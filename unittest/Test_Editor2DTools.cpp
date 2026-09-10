#include <AYEditor/Editor2DViewportModel.h>
#include <AYEditor/EditorSceneCamera.h>
#include <AYEditor/EditorSceneViewWorkspace.h>
#include <AYEditor/EditorTilemapDocument.h>
#include <AYTest.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace ayt::editor;

TEST_SUITE(Editor2DToolsTests)

TEST_CASE(ViewportRoundTripGridPanAndAnchoredZoom)
{
    Editor2DViewportModel viewport;
    viewport.setViewport(800u, 400u);
    viewport.setCamera({10.0f, 20.0f}, 200.0f);
    viewport.setGrid(32.0f, 16.0f, {-16.0f, -8.0f});

    const ayt::math::FVector2 screen{700.0f, 100.0f};
    const ayt::math::FVector2 world = viewport.screenToWorld(screen);
    const ayt::math::FVector2 roundTrip = viewport.worldToScreen(world);
    CHECK_FLOAT_EQ(roundTrip.x, screen.x, 1e-5f);
    CHECK_FLOAT_EQ(roundTrip.y, screen.y, 1e-5f);

    const EditorTileCell cell = viewport.worldToCell({17.0f, 25.0f});
    CHECK_INT_EQ(cell.x, 1);
    CHECK_INT_EQ(cell.y, 2);
    const ayt::math::FVector2 snapped = viewport.snapToGrid({17.0f, 25.0f});
    CHECK_FLOAT_EQ(snapped.x, 16.0f, 1e-6f);
    CHECK_FLOAT_EQ(snapped.y, 24.0f, 1e-6f);

    const ayt::math::FVector2 anchorBefore = viewport.screenToWorld(screen);
    viewport.zoomAt(2.0f, screen);
    const ayt::math::FVector2 anchorAfter = viewport.screenToWorld(screen);
    CHECK_FLOAT_EQ(anchorAfter.x, anchorBefore.x, 1e-5f);
    CHECK_FLOAT_EQ(anchorAfter.y, anchorBefore.y, 1e-5f);
    CHECK_FLOAT_EQ(viewport.verticalWorldSize(), 100.0f, 1e-6f);
}

TEST_CASE(SceneCameraPreservesIndependentTwoDAndThreeDPoses)
{
    EditorSceneCamera camera;
    camera.setViewport(960u, 600u);
    camera.threeD().setPose({8.0f, 7.0f, 6.0f}, 0.4f, -0.2f);
    camera.setTwoDPose({480.0f, 300.0f}, 600.0f);
    camera.setMode(SceneViewMode::TwoD);

    ayt::math::FVector3 origin{};
    ayt::math::FVector3 direction{};
    CHECK_TRUE(camera.ray({480.0f, 300.0f}, origin, direction));
    CHECK_FLOAT_EQ(origin.x, 480.0f, 1e-5f);
    CHECK_FLOAT_EQ(origin.y, 300.0f, 1e-5f);
    CHECK_FLOAT_EQ(direction.z, -1.0f, 1e-5f);

    camera.setMode(SceneViewMode::ThreeD);
    CHECK_FLOAT_EQ(camera.threeD().eye().x, 8.0f, 1e-5f);
    camera.setMode(SceneViewMode::TwoD);
    CHECK_FLOAT_EQ(camera.twoDCenter().x, 480.0f, 1e-5f);
    CHECK_FLOAT_EQ(camera.twoDViewHeight(), 600.0f, 1e-5f);
}

TEST_CASE(SceneViewDefaultPrecedenceUsesProjectContentAndProfile)
{
    EditorSceneContentProfile content;
    CHECK(chooseInitialSceneViewMode("2D", content, "CLIENT_3D")
          == SceneViewMode::TwoD);
    CHECK(chooseInitialSceneViewMode("3D", content, "CLIENT_2D")
          == SceneViewMode::ThreeD);
    content.hasOrthographicCamera = true;
    CHECK(chooseInitialSceneViewMode("Auto", content, "CLIENT_3D")
          == SceneViewMode::TwoD);
    content.hasOrthographicCamera = false;
    content.hasThreeDContent = true;
    CHECK(chooseInitialSceneViewMode("Auto", content, "CLIENT_2D")
          == SceneViewMode::ThreeD);
}

TEST_CASE(SceneViewWorkspaceRoundTripsBothCameraPoses)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "editor_scene_view_workspace_test";
    std::error_code ignored;
    fs::remove_all(root, ignored);

    EditorSceneCameraState state;
    state.mode = SceneViewMode::TwoD;
    state.threeDProjection = ProjectionMode::Orthographic;
    state.twoDCenter = {123.0f, 456.0f};
    state.twoDViewHeight = 321.0f;
    state.threeDEye = {9.0f, 8.0f, 7.0f};

    EditorSceneViewWorkspace written;
    std::string error;
    CHECK_TRUE(written.open(root.string(), &error));
    written.set((root / "Assets/worlds/main.ayscene").string(), state);
    CHECK_TRUE(written.save(&error));

    EditorSceneViewWorkspace loaded;
    CHECK_TRUE(loaded.open(root.string(), &error));
    const EditorSceneCameraState* restored = loaded.find(
        (root / "Assets/worlds/main.ayscene").string());
    CHECK(restored != nullptr);
    CHECK(restored != nullptr && restored->mode == SceneViewMode::TwoD);
    CHECK(restored != nullptr
          && restored->threeDProjection == ProjectionMode::Orthographic);
    if (restored != nullptr) {
        CHECK_FLOAT_EQ(restored->twoDCenter.x, 123.0f, 1e-5f);
        CHECK_FLOAT_EQ(restored->twoDViewHeight, 321.0f, 1e-5f);
        CHECK_FLOAT_EQ(restored->threeDEye.z, 7.0f, 1e-5f);
    }
    fs::remove_all(root, ignored);
}

TEST_CASE(SceneSessionCenterRayKeepsLegacyThreeDPickingContract)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK_FALSE(layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK_TRUE(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);
    auto* viewport = dynamic_cast<ayt::ui::Image*>(
        session.ui().findById("panel_viewport"));
    ayt::entity::World* world =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    CHECK(viewport != nullptr);
    CHECK(world != nullptr);
    if (viewport == nullptr || world == nullptr) {
        session.shutdown();
        return;
    }

    const auto bounds = viewport->getWorldBounds();
    ayt::math::FVector3 origin{};
    ayt::math::FVector3 direction{};
    CHECK_FLOAT_EQ(static_cast<float>(session.sceneCamera().viewportWidth()),
                   bounds.width(), 0.51f);
    CHECK_FLOAT_EQ(static_cast<float>(session.sceneCamera().viewportHeight()),
                   bounds.height(), 0.51f);
    CHECK_TRUE(session.sceneCamera().ray(
        {bounds.width() * 0.5f, bounds.height() * 0.5f},
        origin, direction));
    const ayt::math::FVector3 toOrigin = origin * -1.0f;
    CHECK_FLOAT_EQ(origin.x, session.freecam().eye().x, 1e-5f);
    CHECK_FLOAT_EQ(direction.x, session.freecam().forward().x, 1e-3f);
    CHECK_FLOAT_EQ(direction.y, session.freecam().forward().y, 1e-3f);
    CHECK_FLOAT_EQ(direction.z, session.freecam().forward().z, 1e-3f);
    CHECK_FLOAT_EQ(toOrigin.dot(direction), toOrigin.length(), 1e-3f);

    ayt::entity::Entity* entity = world->createEntity();
    entity->addComponent<ayt::entity::Transform>();
    entity->addComponent<ayt::entity::MeshComponent>();
    const float x = (bounds.minX + bounds.maxX) * 0.5f;
    const float y = (bounds.minY + bounds.maxY) * 0.5f;
    CHECK_TRUE(session.onMouseButtonDown(x, y, 0));
    CHECK_TRUE(session.onMouseButtonUp(x, y, 0));
    CHECK(session.selectedEntityId() == entity->getId());
    session.shutdown();
}

TEST_CASE(TilemapDocumentPaintFillMetadataAndRoundTrip)
{
    const char* path = "editor_2d_tools.aytilemap.json";
    EditorTilemapDocument document;
    CHECK_TRUE(document.create(3u, 2u, 32u, 16u, 0u));
    CHECK_TRUE(document.paint(1u, 0u, 9u));
    CHECK_TRUE(document.paint(1u, 1u, 9u));
    CHECK_INT_EQ(document.floodFill(0u, 0u, 5u), 2u);
    CHECK_INT_EQ(document.tileAt(0u, 1u), 5u);
    CHECK_INT_EQ(document.tileAt(2u, 1u), 0u);
    document.setCollisionFlags(9u, 0x11u);
    document.setAnimation(5u, {{10u, 100u}, {11u, 150u}});
    CHECK_TRUE(document.save(path));
    CHECK_FALSE(document.dirty());

    EditorTilemapDocument loaded;
    std::string error;
    CHECK_TRUE(loaded.load(path, &error));
    CHECK_TRUE(error.empty());
    CHECK_INT_EQ(loaded.cols(), 3u);
    CHECK_INT_EQ(loaded.rows(), 2u);
    CHECK_INT_EQ(loaded.tileAt(0u, 0u), 5u);
    CHECK_INT_EQ(loaded.tileAt(1u, 1u), 9u);
    CHECK_INT_EQ(loaded.collisionFlags().at(9u), 0x11u);
    CHECK_INT_EQ(static_cast<uint32_t>(loaded.animations().at(5u).size()), 2u);
    CHECK_INT_EQ(loaded.animations().at(5u)[1].tileId, 11u);
    CHECK_INT_EQ(loaded.animations().at(5u)[1].durationMs, 150u);
    CHECK_FALSE(loaded.dirty());
    std::remove(path);
}

TEST_CASE(TilemapDocumentRejectsInvalidDimensionsAndOutOfBoundsPaint)
{
    EditorTilemapDocument document;
    CHECK_FALSE(document.create(0u, 2u, 16u, 16u));
    CHECK_TRUE(document.create(2u, 2u, 16u, 16u));
    CHECK_FALSE(document.paint(2u, 0u, 1u));
    CHECK_INT_EQ(document.floodFill(99u, 99u, 1u), 0u);
}

TEST_SUITE_END
