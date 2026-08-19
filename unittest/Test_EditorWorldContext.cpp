#include "AYTest.h"

#include "AYEditor/EditorWorldContext.h"
#include "AYEditor/EditorSession.h"
#include "AYEntity.h"
#include "AYApplication.h"
#include "AYApplication/IEngineHost.h"
#include "AYScene.h"
#include "AYScene/SceneManager.h"
#include "AYUI/MockRenderer.h"

using namespace ayt::editor;

TEST_SUITE(AYEditor_WorldContext)

TEST_CASE(editor_world_context_play_uses_explicit_fallback)
{
    EditorWorldContext context;
    auto* fallback = &ayt::entity::World::instance();

    context.setFallbackWorld(fallback);

    CHECK(context.sceneManager() == nullptr);
    CHECK(context.scene(EditorWorldSlot::Play) == nullptr);
    CHECK(context.world(EditorWorldSlot::Play) == fallback);
    CHECK(context.world(EditorWorldSlot::Play, false) == fallback);
}

TEST_CASE(editor_world_context_edit_never_uses_runtime_fallback)
{
    EditorWorldContext context;
    context.setFallbackWorld(&ayt::entity::World::instance());

    CHECK(context.scene(EditorWorldSlot::Edit) == nullptr);
    CHECK(context.world(EditorWorldSlot::Edit) == nullptr);
    CHECK(context.world(EditorWorldSlot::Edit, false) == nullptr);
}

TEST_CASE(editor_world_context_supports_explicit_fallback_detach)
{
    EditorWorldContext context;
    context.setFallbackWorld(&ayt::entity::World::instance());
    context.setFallbackWorld(nullptr);

    CHECK(context.world(EditorWorldSlot::Play) == nullptr);
}

TEST_CASE(editor_session_binds_and_detaches_host_world_context)
{
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::ui::MockRenderer backend;
    EditorSession session;

    CHECK(session.initialize(&backend, ""));

    auto* scenes = ayt::app::currentEngineHost()->scenes();
    CHECK_NOT_NULL(scenes);
    if (scenes == nullptr) return;
    CHECK(session.worldContext().sceneManager() == scenes);
    auto* editScene = scenes->edit();
    CHECK_NOT_NULL(editScene);
    if (editScene == nullptr) return;
    CHECK(session.worldContext().scene(EditorWorldSlot::Edit) == editScene);
    CHECK(session.worldContext().world(EditorWorldSlot::Edit)
          == &editScene->world());

    session.shutdown();
    CHECK(session.worldContext().sceneManager() == nullptr);
}

TEST_SUITE_END
