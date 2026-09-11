#include <AYEditor/Editor2DViewportModel.h>
#include <AYEditor/EditorSceneCamera.h>
#include <AYEditor/EditorSceneViewWorkspace.h>
#include <AYEditor/EditorProjectRuntimeValidator.h>
#include <AYEditor/EditorTilemapDocument.h>
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorTransformGizmo.h>

#include "EditorGameViewTestAccess.h"
#include <AYEditor/EditorWorkspace.h>
#include <AYEntity/components/OrthoCameraComponent.h>
#include <AYEntity/components/SpriteComponent.h>
#include <AYEntity/components/TilemapComponent.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYUI/ComboBox.h>
#include <AYUI/Button.h>
#include <AYUI/Menu.h>
#include <AYUI/MenuBar.h>
#include <AYUI/MenuItem.h>
#include <AYUI/TextInput.h>
#include <AYUI/UIKeyCode.h>
#include <AYScene.h>
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
    EditorSceneVisibility visibility;
    visibility.meshes = false;
    visibility.worldLit2D = true;
    visibility.cameraOverlay2D = false;
    visibility.ui = true;

    EditorSceneViewWorkspace written;
    std::string error;
    CHECK_TRUE(written.open(root.string(), &error));
    written.set((root / "Assets/worlds/main.ayscene").string(), state);
    written.setVisibility(
        (root / "Assets/worlds/main.ayscene").string(), visibility);
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
    const EditorSceneVisibility* restoredVisibility = loaded.findVisibility(
        (root / "Assets/worlds/main.ayscene").string());
    CHECK(restoredVisibility != nullptr);
    CHECK(restoredVisibility != nullptr && !restoredVisibility->meshes);
    CHECK(restoredVisibility != nullptr && restoredVisibility->worldLit2D);
    CHECK(restoredVisibility != nullptr
          && !restoredVisibility->cameraOverlay2D);
    CHECK(restoredVisibility != nullptr && restoredVisibility->ui);
    fs::remove_all(root, ignored);
}

TEST_CASE(StandardMixedProjectUsesOneSceneAcrossValidationProfiles)
{
    namespace fs = std::filesystem;
    const fs::path projectRoot = fs::path(AY_EDITOR_TEST_SOURCE_DIR)
        .parent_path() / "Validation" / "Mixed2D3DProject";
    const fs::path scenePath = projectRoot / "Assets" / "worlds"
        / "mixed_room.ayscene";

    const EditorRuntimeValidationResult headless =
        EditorProjectRuntimeValidator::validate(
            projectRoot.string(), EditorRuntimeValidationProfile::Headless);
    const EditorRuntimeValidationResult client =
        EditorProjectRuntimeValidator::validate(
            projectRoot.string(), EditorRuntimeValidationProfile::FullClient);
    CHECK_TRUE(static_cast<bool>(headless));
    CHECK_TRUE(static_cast<bool>(client));
    CHECK_INT_EQ(static_cast<uint32_t>(headless.scenes), 1u);
    CHECK_INT_EQ(static_cast<uint32_t>(headless.uiLayouts), 1u);
    CHECK_INT_EQ(static_cast<uint32_t>(client.scenes), 1u);
    CHECK_INT_EQ(static_cast<uint32_t>(client.uiLayouts), 1u);

    ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "mixed-room");
    CHECK_TRUE(scene.load(scenePath.string()));
    bool hasMesh = false;
    bool hasWorldLit = false;
    bool hasOverlay = false;
    bool hasOverlayCamera = false;
    for (ayt::entity::Entity* entity : scene.world().getAllEntities()) {
        if (entity == nullptr) continue;
        hasMesh |= entity->getComponent<ayt::entity::MeshComponent>() != nullptr;
        if (const auto* sprite = entity->getComponent<
                ayt::entity::SpriteComponent>()) {
            hasWorldLit |= sprite->renderDomain == 1;
            hasOverlay |= sprite->renderDomain == 0;
        }
        hasOverlayCamera |= entity->getComponent<
            ayt::entity::OrthoCameraComponent>() != nullptr;
    }
    CHECK(hasMesh);
    CHECK(hasWorldLit);
    CHECK(hasOverlay);
    CHECK(hasOverlayCamera);
}

TEST_CASE(SceneViewFiltersPersistAndUiPreviewIsPassive)
{
    namespace fs = std::filesystem;
    const std::string layoutPath = resolveEditorShellLayoutPath();
    if (layoutPath.empty()) return;
    const fs::path sourceRoot = fs::path(AY_EDITOR_TEST_SOURCE_DIR)
        .parent_path() / "Validation" / "Mixed2D3DProject";
    const fs::path projectRoot = fs::current_path()
        / "editor_mixed_scene_visibility_test";
    std::error_code ignored;
    fs::remove_all(projectRoot, ignored);
    fs::copy(sourceRoot, projectRoot, fs::copy_options::recursive);

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = layoutPath;
    desc.projectRoot = projectRoot.string();
    desc.engineAssetsRoot = fs::path(AY_EDITOR_TEST_SOURCE_DIR)
        .parent_path().string();
    desc.iconRootPath = (fs::path(desc.engineAssetsRoot)
        / "Icons" / "Tabler").string();
    EditorSession session;
    CHECK_TRUE(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);

    ayt::ui::Widget* preview = nullptr;
    for (ayt::ui::Widget* child : session.ui().getOverlayRoot()->getChildren()) {
        if (child != nullptr && child->getId() == "scene_ui_preview_host") {
            preview = child;
            break;
        }
    }
    CHECK(preview != nullptr
          && preview->getId() == "scene_ui_preview_host");
    ayt::ui::Widget* hud = preview != nullptr
        && !preview->getChildren().empty()
        ? preview->getChildren().front() : nullptr;
    CHECK(hud != nullptr && hud->getId() == "mixed_hud_root");
    CHECK(session.ui().findById("mixed_hud_root") == nullptr);
    CHECK(preview != nullptr && preview->hitTest({640.0f, 360.0f}) == nullptr);

    const SceneViewMode modeBefore = session.sceneViewMode();
    EditorSceneVisibility visibility;
    visibility.meshes = false;
    visibility.worldLit2D = true;
    visibility.cameraOverlay2D = false;
    visibility.ui = false;
    session.setSceneVisibility(visibility);
    CHECK(session.sceneViewMode() == modeBefore);
    CHECK_FALSE(session.sceneVisibility().meshes);
    CHECK(session.sceneVisibility().worldLit2D);
    CHECK_FALSE(session.sceneVisibility().cameraOverlay2D);
    CHECK_FALSE(session.sceneVisibility().ui);
    CHECK(preview != nullptr && !preview->isVisible());

    auto* meshButton = dynamic_cast<ayt::ui::Button*>(
        session.ui().findById("btn_view_meshes"));
    auto* worldLitButton = dynamic_cast<ayt::ui::Button*>(
        session.ui().findById("btn_view_world_lit_2d"));
    auto* overlayButton = dynamic_cast<ayt::ui::Button*>(
        session.ui().findById("btn_view_camera_overlay_2d"));
    auto* uiButton = dynamic_cast<ayt::ui::Button*>(
        session.ui().findById("btn_view_ui"));
    CHECK(meshButton != nullptr && meshButton->getIconDocument() != nullptr);
    CHECK(worldLitButton != nullptr
          && worldLitButton->getIconDocument() != nullptr);
    CHECK(overlayButton != nullptr
          && overlayButton->getIconDocument() != nullptr);
    CHECK(uiButton != nullptr && uiButton->getIconDocument() != nullptr);
    CHECK(meshButton != nullptr
          && meshButton->getAccessibilityLabel() == L"3D meshes: hidden");
    CHECK(worldLitButton != nullptr
          && worldLitButton->getAccessibilityLabel()
              == L"World-lit 2D content: visible");

    if (meshButton != nullptr) {
        const auto bounds = meshButton->getWorldBounds();
        const float buttonX = (bounds.minX + bounds.maxX) * 0.5f;
        const float buttonY = (bounds.minY + bounds.maxY) * 0.5f;
        CHECK_TRUE(session.onMouseButtonDown(buttonX, buttonY, 0));
        CHECK_TRUE(session.onMouseButtonUp(buttonX, buttonY, 0));
        CHECK(session.sceneVisibility().meshes);
        CHECK_TRUE(session.onMouseButtonDown(buttonX, buttonY, 0));
        CHECK_TRUE(session.onMouseButtonUp(buttonX, buttonY, 0));
        CHECK_FALSE(session.sceneVisibility().meshes);
    }

    // The passive UI preview occupies the complete Scene View rectangle. Its
    // overlay bounds must not classify the native viewport as editor chrome;
    // exercise the real RMB pan route instead of only checking host::hitTest.
    if (session.sceneViewMode() != SceneViewMode::TwoD) {
        auto* modeButton = dynamic_cast<ayt::ui::Button*>(
            session.ui().findById("btn_view_mode"));
        CHECK(modeButton != nullptr);
        if (modeButton != nullptr) {
            const auto bounds = modeButton->getWorldBounds();
            const float buttonX = (bounds.minX + bounds.maxX) * 0.5f;
            const float buttonY = (bounds.minY + bounds.maxY) * 0.5f;
            CHECK_TRUE(session.onMouseButtonDown(buttonX, buttonY, 0));
            CHECK_TRUE(session.onMouseButtonUp(buttonX, buttonY, 0));
        }
    }
    CHECK(session.sceneViewMode() == SceneViewMode::TwoD);
    ayt::math::FRectangle viewportBounds{};
    CHECK_TRUE(session.getViewportBounds(viewportBounds));
    const float dragX = (viewportBounds.minX + viewportBounds.maxX) * 0.5f;
    const float dragY = (viewportBounds.minY + viewportBounds.maxY) * 0.5f;
    const ayt::math::FVector2 centerBeforeDrag = session.sceneCamera().twoDCenter();
    CHECK_TRUE(session.onMouseButtonDown(dragX, dragY, 1));
    CHECK_TRUE(session.onMouseMove(dragX + 24.0f, dragY + 12.0f));
    CHECK_TRUE(session.onMouseButtonUp(dragX + 24.0f, dragY + 12.0f, 1));
    const ayt::math::FVector2 centerAfterDrag = session.sceneCamera().twoDCenter();
    CHECK(centerAfterDrag.x != centerBeforeDrag.x
          || centerAfterDrag.y != centerBeforeDrag.y);

    EditorSceneDocument* editDocument = session.document();
    const std::size_t editEntityCount = editDocument != nullptr
        ? editDocument->scene().world().getAllEntities().size() : 0u;
    visibility.ui = true;
    session.setSceneVisibility(visibility, false);
    CHECK(preview != nullptr && preview->isVisible());
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Play);
    CHECK(session.gameView().mode() == EditorMode::Play);
    CHECK(session.document() == editDocument);
    CHECK(editDocument != nullptr
          && editDocument->scene().world().getAllEntities().size()
              == editEntityCount);
    CHECK(preview != nullptr && !preview->isVisible());
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Edit);
    CHECK(session.gameView().mode() == EditorMode::Edit);
    CHECK(preview != nullptr && preview->isVisible());
    visibility.ui = false;
    session.setSceneVisibility(visibility);

    session.shutdown();

    EditorSceneViewWorkspace workspace;
    std::string error;
    CHECK_TRUE(workspace.open(projectRoot.string(), &error));
    const EditorSceneVisibility* restored = workspace.findVisibility(
        (projectRoot / "Assets" / "worlds" / "mixed_room.ayscene").string());
    CHECK(restored != nullptr);
    CHECK(restored != nullptr && !restored->meshes);
    CHECK(restored != nullptr && restored->worldLit2D);
    CHECK(restored != nullptr && !restored->cameraOverlay2D);
    CHECK(restored != nullptr && !restored->ui);
    fs::remove_all(projectRoot, ignored);
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

TEST_CASE(TwoDSceneTemplateAndCreateCommandsBuildEditableEntities)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK_FALSE(layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK_TRUE(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);
    auto* menuBar = dynamic_cast<ayt::ui::MenuBar*>(
        session.ui().findById("menubar"));
    bool hasEmptyTemplate = false;
    bool hasTwoDTemplate = false;
    bool hasCreateSprite = false;
    bool hasCreateTilemap = false;
    bool hasCreateCamera = false;
    if (menuBar != nullptr) {
        for (std::size_t menuIndex = 0;
             menuIndex < menuBar->getMenuCount(); ++menuIndex) {
            ayt::ui::Menu* menu = menuBar->getMenu(menuIndex);
            if (menu == nullptr) continue;
            for (std::size_t itemIndex = 0;
                 itemIndex < menu->getItemCount(); ++itemIndex) {
                ayt::ui::MenuItem* item = menu->getItem(
                    static_cast<int>(itemIndex));
                if (item == nullptr) continue;
                hasEmptyTemplate |= item->getText() == L"New Empty Scene";
                hasTwoDTemplate |= item->getText() == L"New 2D Scene";
                hasCreateSprite |= item->getText() == L"Create Sprite";
                hasCreateTilemap |= item->getText() == L"Create Tilemap";
                hasCreateCamera |= item->getText() == L"Create 2D Camera";
            }
        }
    }
    CHECK(menuBar != nullptr);
    CHECK(hasEmptyTemplate);
    CHECK(hasTwoDTemplate);
    CHECK(hasCreateSprite);
    CHECK(hasCreateTilemap);
    CHECK(hasCreateCamera);
    CHECK_TRUE(session.newSceneFromTemplate(EditorSceneTemplate::TwoD));
    CHECK(session.sceneViewMode() == SceneViewMode::TwoD);

    ayt::entity::World* world =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    CHECK(world != nullptr);
    if (world == nullptr) {
        session.shutdown();
        return;
    }
    CHECK_INT_EQ(static_cast<uint32_t>(world->getAllEntities().size()), 1u);
    ayt::entity::Entity* cameraEntity = world->getAllEntities().front();
    CHECK(cameraEntity->getComponent<ayt::entity::Transform>() != nullptr);
    auto* camera = cameraEntity->getComponent<
        ayt::entity::OrthoCameraComponent>();
    CHECK(camera != nullptr);
    CHECK(camera != nullptr && camera->viewSize == 600.0f);

    const uint32_t spriteId =
        session.createTwoDEntity(Editor2DEntityKind::Sprite);
    const uint32_t tilemapId =
        session.createTwoDEntity(Editor2DEntityKind::Tilemap);
    CHECK(spriteId != 0u);
    CHECK(tilemapId != 0u);
    CHECK(world->findEntity(spriteId)->getComponent<
        ayt::entity::SpriteComponent>() != nullptr);
    CHECK(world->findEntity(tilemapId)->getComponent<
        ayt::entity::TilemapComponent>() != nullptr);
    CHECK(session.document()->isDirty());
    session.shutdown();
}

TEST_CASE(TwoDInspectorUsesResourceAndEnumControls)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    if (layoutPath.empty()) return;
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK_TRUE(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    (void)session.createTwoDEntity(Editor2DEntityKind::Sprite);
    auto* textureRow = session.ui().findById("inspector_field_texturePath");
    auto* flip = dynamic_cast<ayt::ui::ComboBox*>(
        session.ui().findById("inspector_field_flip"));
    auto* domain = dynamic_cast<ayt::ui::ComboBox*>(
        session.ui().findById("inspector_field_renderDomain"));
    auto* layer = dynamic_cast<ayt::ui::TextInput*>(
        session.ui().findById("inspector_field_layer"));
    CHECK(textureRow != nullptr);
    CHECK(flip != nullptr);
    CHECK(domain != nullptr);
    CHECK(layer != nullptr);
    CHECK(flip != nullptr && flip->getItemCount() == 4u);
    CHECK(domain != nullptr && domain->getItemCount() == 2u);

    (void)session.createTwoDEntity(Editor2DEntityKind::Camera);
    auto* aspectPolicy = dynamic_cast<ayt::ui::ComboBox*>(
        session.ui().findById("inspector_field_aspectPolicy"));
    auto* zoom = dynamic_cast<ayt::ui::TextInput*>(
        session.ui().findById("inspector_field_zoom"));
    auto* viewSize = dynamic_cast<ayt::ui::TextInput*>(
        session.ui().findById("inspector_field_viewSize"));
    CHECK(aspectPolicy != nullptr);
    CHECK(aspectPolicy != nullptr && aspectPolicy->getItemCount() == 3u);
    CHECK(zoom != nullptr);
    CHECK(viewSize != nullptr);
    session.shutdown();
}

TEST_CASE(TwoDPickingUsesDrawableLayerOrder)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    if (layoutPath.empty()) return;
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK_TRUE(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);
    CHECK_TRUE(session.newSceneFromTemplate(EditorSceneTemplate::TwoD));

    ayt::entity::World* world =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    const uint32_t lowerId =
        session.createTwoDEntity(Editor2DEntityKind::Sprite);
    const uint32_t upperId =
        session.createTwoDEntity(Editor2DEntityKind::Sprite);
    auto* lower = world->findEntity(lowerId)->getComponent<
        ayt::entity::SpriteComponent>();
    auto* upper = world->findEntity(upperId)->getComponent<
        ayt::entity::SpriteComponent>();
    lower->layer = 1;
    upper->layer = 3;

    ayt::math::FRectangle bounds{};
    CHECK_TRUE(session.getViewportBounds(bounds));
    const float centerX = (bounds.minX + bounds.maxX) * 0.5f;
    const float centerY = (bounds.minY + bounds.maxY) * 0.5f;
    // Clear the current selection away from every drawable, then pick the
    // overlapping sprites. This avoids the current gizmo consuming its own
    // center handle before the selection test runs.
    CHECK_TRUE(session.onMouseButtonDown(centerX + 200.0f, centerY, 0));
    CHECK_TRUE(session.onMouseButtonUp(centerX + 200.0f, centerY, 0));
    CHECK_TRUE(session.onMouseButtonDown(centerX + 10.0f, centerY, 0));
    CHECK_TRUE(session.onMouseButtonUp(centerX + 10.0f, centerY, 0));
    CHECK_INT_EQ(session.selectedEntityId(), upperId);
    session.shutdown();
}

TEST_CASE(TwoDGizmoUsesExplicitScreenStableScale)
{
    EditorTransformGizmo gizmo;
    const EditorTransformState transform{
        {0.0f, 0.0f, 0.0f}, ayt::math::FQuaternion::identity(),
        {1.0f, 1.0f, 1.0f}};
    constexpr float scale = 72.0f;
    const uint16_t disabled =
        EditorTransformGizmo::handleBit(EditorGizmoHandle::AxisZ)
      | EditorTransformGizmo::handleBit(EditorGizmoHandle::PlaneYZ)
      | EditorTransformGizmo::handleBit(EditorGizmoHandle::PlaneZX)
      | EditorTransformGizmo::handleBit(EditorGizmoHandle::RingX)
      | EditorTransformGizmo::handleBit(EditorGizmoHandle::RingY)
      | EditorTransformGizmo::handleBit(EditorGizmoHandle::ScaleZ);
    const ayt::math::FVector3 rayOrigin{scale * 0.75f, 0.0f, 1.0f};
    const ayt::math::FVector3 rayDirection{0.0f, 0.0f, -1.0f};
    CHECK(gizmo.hitTestUniversal(transform, false, rayOrigin, rayDirection,
                                 disabled, scale)
          == EditorGizmoHandle::AxisX);
    CHECK_TRUE(gizmo.beginUniversal(EditorGizmoHandle::AxisX, transform,
                                    false, rayOrigin, rayDirection, 0.0f,
                                    disabled, scale));
    EditorTransformState moved;
    CHECK_TRUE(gizmo.update({rayOrigin.x + 10.0f, rayOrigin.y, rayOrigin.z},
                            rayDirection, 0.0f, 0.0f, moved));
    CHECK_FLOAT_EQ(moved.position.x, 10.0f, 1e-4f);
}

TEST_CASE(TwoDGizmoTransformCommitsThroughUndoRedo)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    if (layoutPath.empty()) return;
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK_TRUE(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);
    CHECK_TRUE(session.newSceneFromTemplate(EditorSceneTemplate::TwoD));
    const uint32_t spriteId =
        session.createTwoDEntity(Editor2DEntityKind::Sprite);
    ayt::entity::World* world =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    auto* transform = world->findEntity(spriteId)->getComponent<
        ayt::entity::Transform>();
    ayt::math::FRectangle bounds{};
    CHECK_TRUE(session.getViewportBounds(bounds));
    const float centerX = (bounds.minX + bounds.maxX) * 0.5f;
    const float centerY = (bounds.minY + bounds.maxY) * 0.5f;
    const float handleX = centerX + 72.0f * 0.75f;
    CHECK_TRUE(session.onMouseButtonDown(handleX, centerY, 0));
    CHECK_TRUE(session.onMouseButtonUp(handleX + 16.0f, centerY, 0));
    CHECK(transform->position.x > 1.0f);
    const float movedX = transform->position.x;

    session.workspace().commands().setActiveTarget(nullptr);
    // The scene command stack is registered as the active target by normal
    // editor input. Undo/redo shortcuts use the same command router path.
    session.onKeyDown(ayt::ui::UIKey_Control);
    CHECK_TRUE(session.onKeyDown(ayt::ui::UIKey_Z));
    session.onKeyUp(ayt::ui::UIKey_Control);
    CHECK_FLOAT_EQ(transform->position.x, 0.0f, 1e-4f);
    session.onKeyDown(ayt::ui::UIKey_Control);
    CHECK_TRUE(session.onKeyDown(ayt::ui::UIKey_Y));
    session.onKeyUp(ayt::ui::UIKey_Control);
    CHECK_FLOAT_EQ(transform->position.x, movedX, 1e-4f);
    session.shutdown();
}

TEST_CASE(TwoDAssetsPlaceAsSpriteAndCookedTilemapReferences)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "editor_2d_asset_drop_test";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root / "Assets" / "textures", ignored);
    fs::create_directories(root / "Assets" / "tilemaps", ignored);
    std::FILE* image = std::fopen(
        (root / "Assets" / "textures" / "marker.png").string().c_str(), "wb");
    CHECK(image != nullptr);
    if (image != nullptr) std::fclose(image);
    EditorTilemapDocument tilemap;
    CHECK_TRUE(tilemap.create(4u, 2u, 16u, 8u, 0u));
    std::string error;
    CHECK_TRUE(tilemap.save(
        (root / "Assets" / "tilemaps" / "ground.aytilemap.json").string(),
        &error));

    const std::string layoutPath = resolveEditorShellLayoutPath();
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = layoutPath;
    desc.projectRoot = root.string();
    EditorSession session;
    CHECK_TRUE(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);
    CHECK_TRUE(session.newSceneFromTemplate(EditorSceneTemplate::TwoD));
    CHECK_TRUE(session.assetDatabase().scanNow(&error));
    const EditorAssetRecord* texture = session.assetDatabase().findByLogicalPath(
        "Assets/textures/marker.png");
    const EditorAssetRecord* sourceTilemap =
        session.assetDatabase().findByLogicalPath(
            "Assets/tilemaps/ground.aytilemap.json");
    CHECK(texture != nullptr);
    CHECK(sourceTilemap != nullptr);

    ayt::math::FRectangle bounds{};
    CHECK_TRUE(session.getViewportBounds(bounds));
    const float x = (bounds.minX + bounds.maxX) * 0.5f;
    const float y = (bounds.minY + bounds.maxY) * 0.5f;
    CHECK(texture != nullptr
          && session.placeAssetInViewport(texture->id, x, y));
    ayt::entity::World* world =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    auto* spriteEntity = world->findEntity(session.selectedEntityId());
    auto* sprite = spriteEntity != nullptr
        ? spriteEntity->getComponent<ayt::entity::SpriteComponent>() : nullptr;
    CHECK(sprite != nullptr);
    CHECK(sprite != nullptr
          && sprite->texturePath == "textures/marker.png");

    CHECK(sourceTilemap != nullptr
          && session.placeAssetInViewport(sourceTilemap->id, x, y));
    auto* tilemapEntity = world->findEntity(session.selectedEntityId());
    auto* tilemapComponent = tilemapEntity != nullptr
        ? tilemapEntity->getComponent<ayt::entity::TilemapComponent>() : nullptr;
    CHECK(tilemapComponent != nullptr);
    CHECK(tilemapComponent != nullptr
          && tilemapComponent->tilemapPath == "tilemaps/ground.aytilemap");

    fs::create_directories(root / "Assets" / "worlds", ignored);
    const fs::path scenePath = root / "Assets" / "worlds" / "stage4.ayscene";
    CHECK_TRUE(session.document()->saveAs(scenePath.string(), &error));
    EditorSceneDocument reopened;
    CHECK_TRUE(reopened.open(scenePath.string(), &error));
    uint32_t reopenedSprites = 0u;
    uint32_t reopenedTilemaps = 0u;
    uint32_t reopenedCameras = 0u;
    for (ayt::entity::Entity* entity :
         reopened.scene().world().getAllEntities()) {
        if (entity->getComponent<ayt::entity::SpriteComponent>() != nullptr) {
            ++reopenedSprites;
        }
        if (entity->getComponent<ayt::entity::TilemapComponent>() != nullptr) {
            ++reopenedTilemaps;
        }
        if (entity->getComponent<ayt::entity::OrthoCameraComponent>() != nullptr) {
            ++reopenedCameras;
        }
    }
    CHECK_INT_EQ(reopenedSprites, 1u);
    CHECK_INT_EQ(reopenedTilemaps, 1u);
    CHECK_INT_EQ(reopenedCameras, 1u);
    session.shutdown();
    fs::remove_all(root, ignored);
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
