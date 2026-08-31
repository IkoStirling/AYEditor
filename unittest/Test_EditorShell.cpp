#include "AYTest.h"
#include "AYEditor/EditorSession.h"
#include "AYEditor/EditorVisualStyle.h"
#include "EditorGameViewTestAccess.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/Box.h"
#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "AYUI/Button.h"
#include "AYUI/CheckBox.h"
#include "AYUI/ComboBox.h"
#include "AYUI/Slider.h"
#include "AYUI/TextLabel.h"  // PR-5 LM-2 test: hint TextLabel observe
#include "AYUI/Image.h"
#include "AYUI/MenuBar.h"
#include "AYUI/TextInput.h"
#include "AYUI/Theme.h"
#include "AYEntity.h"
#include <AYEntity/components/MeshComponent.h>
#include "AYApplication/IEngineHost.h"
#include "AYApplication.h"
#include "AYScene.h"
#include "AYScene/SceneManager.h"
#include "AYDevice/KeyboardDevice.h"

#include <cmath>
#include <sys/stat.h>
#include <string>

using namespace ayt::ui;
using namespace ayt::editor;

namespace {

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

std::string resolveEditorShellLayoutPath()
{
    const std::string candidates[] = {
        AY_EDITOR_TEST_SOURCE_DIR "/assets/ui/editor_shell.ui.json",
        "assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };
    for (const std::string& path : candidates) {
        if (fileExists(path)) return path;
    }
    return {};
}

bool clickButton(Button* button)
{
    if (button == nullptr) return false;
    const auto bounds = button->getWorldBounds();
    const UIMouseEvent event(
        ayt::math::FVector2((bounds.minX + bounds.maxX) * 0.5f,
                           (bounds.minY + bounds.maxY) * 0.5f),
        0);
    return button->onMouseButtonDown(event) &&
           button->onMouseButtonUp(event);
}

bool clickSessionButton(EditorSession& session, Button* button)
{
    if (button == nullptr) return false;
    const auto bounds = button->getWorldBounds();
    const float x = (bounds.minX + bounds.maxX) * 0.5f;
    const float y = (bounds.minY + bounds.maxY) * 0.5f;
    session.onMouseMove(x, y);
    return session.onMouseButtonDown(x, y, 0)
        && session.onMouseButtonUp(x, y, 0);
}

} // namespace

TEST_SUITE(AYEditor_Shell)

TEST_CASE(test_editor_shell_layout_ids) {
    MockRenderer backend;
    UIManager ui;
    ui.initialize(&backend);
    ui.bindEvent("btn_icon_1", "onClick", []() {});

    const char* json = R"({
        "type": "VBox",
        "id": "editor_root",
        "size": { "w": 800, "h": 600 },
        "children": [
            {
                "type": "HBox",
                "id": "menubar_row",
                "size": { "h": 26 },
                "children": [
                    { "type": "MenuBar", "id": "menubar", "size": { "w": 200, "h": 26 } },
                    { "type": "Button", "id": "btn_close", "text": "X" }
                ]
            },
            {
                "type": "HBox",
                "id": "transport_bar",
                "size": { "h": 30 },
                "children": [
                    { "type": "Button", "id": "btn_play", "text": "Play" },
                    { "type": "TextLabel", "id": "lbl_mode", "text": "EDIT" }
                ]
            }
        ]
    })";

    CHECK(ui.loadFromString(json));
    CHECK(ui.findById("menubar") != nullptr);
    CHECK(ui.findById("btn_play") != nullptr);
    CHECK(ui.findById("lbl_mode") != nullptr);
    ui.shutdown();
}

TEST_CASE(test_editor_session_loads_shell_json) {
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    CHECK(session.ui().findById("menubar") != nullptr);
    CHECK(session.ui().findById("app_logo") != nullptr);
    CHECK(session.ui().findById("btn_play") != nullptr);
    CHECK(session.ui().findById("btn_close") != nullptr);
    CHECK(session.ui().findById("panel_viewport") != nullptr);
    CHECK(session.ui().findById("main_dock") != nullptr);
    CHECK(session.ui().findById("card_render") != nullptr);
    CHECK(session.ui().findById("card_inspector") != nullptr);
    CHECK(session.ui().findById("workspace_toolbar") != nullptr);
    CHECK(session.ui().findById("viewport_toolbar") != nullptr);
    CHECK(session.ui().findById("card_assets") != nullptr);
    CHECK(session.ui().findById("card_console") != nullptr);
    CHECK(session.ui().findById("editor_status_bar") != nullptr);
    CHECK(dynamic_cast<CheckBox*>(session.ui().findById("chk_bloom")) != nullptr);
    CHECK(dynamic_cast<CheckBox*>(session.ui().findById("chk_depth_haze")) != nullptr);
    CHECK(dynamic_cast<CheckBox*>(session.ui().findById("chk_ssao")) != nullptr);
    CHECK(dynamic_cast<CheckBox*>(session.ui().findById("chk_fxaa")) != nullptr);
    CHECK(dynamic_cast<CheckBox*>(session.ui().findById("chk_color_grading")) != nullptr);
    CHECK(dynamic_cast<ComboBox*>(session.ui().findById("cmb_color_grading_preset")) != nullptr);
    CHECK(dynamic_cast<Slider*>(session.ui().findById("sld_color_grading_strength")) != nullptr);
    CHECK(dynamic_cast<CheckBox*>(session.ui().findById("chk_shadows")) != nullptr);
    session.shutdown();
}

TEST_CASE(editor_color_grading_enable_promotes_neutral_to_visible_warm_preset) {
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    auto* enabled = dynamic_cast<CheckBox*>(
        session.ui().findById("chk_color_grading"));
    auto* preset = dynamic_cast<ComboBox*>(
        session.ui().findById("cmb_color_grading_preset"));
    CHECK_NOT_NULL(enabled);
    CHECK_NOT_NULL(preset);
    if (enabled != nullptr && preset != nullptr) {
        enabled->setChecked(false);
        preset->setSelectedIndex(0);
        CHECK(preset->getSelectedItem() == L"Neutral (Bypass)");
        enabled->setChecked(true);
        CHECK(preset->getSelectedIndex() == 1);
        CHECK(preset->getSelectedItem() == L"Warm");
    }
    session.shutdown();
}

TEST_CASE(editor_freecam_wheel_zoom_dollies_along_view_direction) {
    EditorFreecam camera;
    const ayt::math::FVector3 initialEye = camera.eye();
    const ayt::math::FVector3 viewDirection = camera.forward();

    camera.zoom(1.0f);
    const ayt::math::FVector3 forwardDelta = camera.eye() - initialEye;
    CHECK_FLOAT_EQ(forwardDelta.dot(viewDirection), 0.75f, 1.0e-5f);
    CHECK_FLOAT_EQ(forwardDelta.lengthSq(), 0.5625f, 1.0e-5f);

    camera.zoom(-1.0f);
    const ayt::math::FVector3 restoredEye = camera.eye();
    CHECK_FLOAT_EQ(restoredEye.x, initialEye.x, 1.0e-5f);
    CHECK_FLOAT_EQ(restoredEye.y, initialEye.y, 1.0e-5f);
    CHECK_FLOAT_EQ(restoredEye.z, initialEye.z, 1.0e-5f);
}

TEST_CASE(editor_freecam_wheel_zoom_can_anchor_to_cursor_ray) {
    EditorFreecam camera;
    const ayt::math::FVector3 initialEye = camera.eye();
    const ayt::math::FVector3 cursorRay(2.0f, -1.0f, -2.0f);
    const ayt::math::FVector3 direction = cursorRay.normalize();

    camera.zoomToward(0.5f, cursorRay);
    const ayt::math::FVector3 delta = camera.eye() - initialEye;
    CHECK_FLOAT_EQ(delta.x, direction.x * 0.375f, 1.0e-5f);
    CHECK_FLOAT_EQ(delta.y, direction.y * 0.375f, 1.0e-5f);
    CHECK_FLOAT_EQ(delta.z, direction.z * 0.375f, 1.0e-5f);
}

TEST_CASE(editor_freecam_w_moves_along_view_and_control_is_not_camera_motion) {
    EditorFreecam camera;
    ayt::device::KeyboardDevice keyboard;
    const ayt::math::FVector3 initialEye = camera.eye();
    const ayt::math::FVector3 viewDirection = camera.forward().normalize();

    keyboard.onKeyDown(ayt::device::KeyCode::W);
    camera.updateMovement(0.5f, keyboard);
    const ayt::math::FVector3 delta = camera.eye() - initialEye;
    CHECK_FLOAT_EQ(delta.dot(viewDirection), 3.0f, 1.0e-5f);
    CHECK_FLOAT_EQ(delta.lengthSq(), 9.0f, 1.0e-4f);
    CHECK(std::fabs(delta.y) > 0.1f);

    keyboard.onKeyUp(ayt::device::KeyCode::W);
    const ayt::math::FVector3 beforeControl = camera.eye();
    keyboard.onKeyDown(ayt::device::KeyCode::LeftControl);
    camera.updateMovement(1.0f, keyboard);
    CHECK_FLOAT_EQ(camera.eye().x, beforeControl.x, 1.0e-5f);
    CHECK_FLOAT_EQ(camera.eye().y, beforeControl.y, 1.0e-5f);
    CHECK_FLOAT_EQ(camera.eye().z, beforeControl.z, 1.0e-5f);
}

TEST_CASE(editor_shell_v2_has_stable_workspace_regions) {
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    Widget* menuRow = session.ui().findById("menubar_row");
    Widget* toolbar = session.ui().findById("workspace_toolbar");
    Widget* dockWidget = session.ui().findById("main_dock");
    Widget* status = session.ui().findById("editor_status_bar");
    Widget* viewportToolbar = session.ui().findById("viewport_toolbar");
    Widget* viewport = session.ui().findById("panel_viewport");
    CHECK_NOT_NULL(menuRow);
    CHECK_NOT_NULL(toolbar);
    CHECK_NOT_NULL(dockWidget);
    CHECK_NOT_NULL(status);
    CHECK_NOT_NULL(viewportToolbar);
    CHECK_NOT_NULL(viewport);
    if (menuRow && toolbar && dockWidget && status
        && viewportToolbar && viewport) {
        CHECK(menuRow->getWorldBounds().maxY <= toolbar->getWorldBounds().minY);
        CHECK(toolbar->getWorldBounds().maxY <= dockWidget->getWorldBounds().minY);
        CHECK(dockWidget->getWorldBounds().maxY <= status->getWorldBounds().minY);
        CHECK(viewportToolbar->getWorldBounds().maxY <= viewport->getWorldBounds().minY);
        CHECK(viewport->getSize().x > 0.0f);
        CHECK(viewport->getSize().y > 0.0f);
    }

    auto* dock = dynamic_cast<DockArea*>(dockWidget);
    CHECK_NOT_NULL(dock);
    if (dock != nullptr) {
        CHECK_FLOAT_EQ(dock->getSlotWeight(DockArea::Slot::Left), 0.20f, 1e-5f);
        CHECK_FLOAT_EQ(dock->getSlotWeight(DockArea::Slot::Right), 0.25f, 1e-5f);
        CHECK_FLOAT_EQ(dock->getSlotWeight(DockArea::Slot::Center), 0.55f, 1e-5f);
        CHECK_FLOAT_EQ(dock->getSlotWeight(DockArea::Slot::Bottom), 0.34f, 1e-5f);

        auto* render = dock->findCard("card_render");
        auto* inspector = dock->findCard("card_inspector");
        auto* assets = dock->findCard("card_assets");
        auto* console = dock->findCard("card_console");
        CHECK_NOT_NULL(render);
        CHECK_NOT_NULL(inspector);
        CHECK_NOT_NULL(assets);
        CHECK_NOT_NULL(console);
        if (render && inspector) CHECK(render->getParent() == inspector->getParent());
        if (assets && console) CHECK(assets->getParent() == console->getParent());
        if (assets) {
            CHECK(assets->isClosable());
            CHECK_FALSE(assets->isFloatable());
        }
        if (console) {
            CHECK(console->isClosable());
            CHECK_FALSE(console->isFloatable());
        }
    }

    session.shutdown();
}

TEST_CASE(editor_menu_bar_stays_inside_top_chrome_row) {
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    Widget* row = session.ui().findById("menubar_row");
    auto* bar = dynamic_cast<MenuBar*>(session.ui().findById("menubar"));
    CHECK_NOT_NULL(row);
    CHECK_NOT_NULL(bar);
    if (row != nullptr && bar != nullptr) {
        const auto rowBounds = row->getWorldBounds();
        const auto barBounds = bar->getWorldBounds();
        CHECK(bar->getSize().y == 20.0f);
        CHECK(barBounds.minY >= rowBounds.minY);
        CHECK(barBounds.maxY <= rowBounds.maxY);
        for (Widget* child : bar->getChildren()) {
            if (auto* anchor = dynamic_cast<Button*>(child)) {
                CHECK(anchor->getSize().y == 20.0f);
                CHECK(anchor->getWorldBounds().maxY <= rowBounds.maxY);
            }
        }
    }

    session.shutdown();
}

TEST_CASE(inspector_panel_renders_pick_apply_reset_widgets) {
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    CHECK_NOT_NULL(session.ui().findById("inspector_hint"));
    CHECK_NOT_NULL(session.ui().findById("inspector_mesh"));
    CHECK_NOT_NULL(session.ui().findById("inspector_skel"));
    CHECK_NOT_NULL(session.ui().findById("inspector_anim"));

    auto requireBtn = [&](const char* id) {
        auto* w = session.ui().findById(id);
        CHECK_NOT_NULL(w);
        auto* b = dynamic_cast<Button*>(w);
        CHECK_NOT_NULL(b);
    };
    requireBtn("btn_inspector_skel");
    requireBtn("btn_inspector_anim");
    requireBtn("btn_inspector_apply");
    requireBtn("btn_inspector_reset");

    session.shutdown();
}

TEST_CASE(test_editor_session_dock_cards_floatable) {
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* dock = dynamic_cast<DockArea*>(session.ui().findById("main_dock"));
    CHECK(dock != nullptr);
    auto* renderCard = dynamic_cast<DockCard*>(session.ui().findById("card_render"));
    auto* viewportCard = dynamic_cast<DockCard*>(session.ui().findById("card_viewport"));
    CHECK(renderCard != nullptr);
    CHECK(viewportCard != nullptr);
    CHECK(renderCard->isFloatable());
    CHECK(!viewportCard->isFloatable());

    session.shutdown();
}

TEST_CASE(renderer_settings_close_and_window_menu_reopen_keeps_live_card) {
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* dock = dynamic_cast<DockArea*>(session.ui().findById("main_dock"));
    auto* render = dynamic_cast<DockCard*>(session.ui().findById("card_render"));
    auto* menuBar = dynamic_cast<MenuBar*>(session.ui().findById("menubar"));
    CHECK_NOT_NULL(dock);
    CHECK_NOT_NULL(render);
    CHECK_NOT_NULL(menuBar);
    if (dock == nullptr || render == nullptr || menuBar == nullptr) {
        session.shutdown();
        return;
    }
    Widget* const renderContent = render->getContent();

    CHECK(dock->requestCloseCard(render));
    CHECK_FALSE(render->isVisible());
    CHECK(dock->findCard("card_render") == render);
    CHECK(session.ui().findById("card_render") == render);
    CHECK(render->getContent() == renderContent);

    Menu* windowMenu = nullptr;
    for (size_t i = 0; i < menuBar->getMenuCount(); ++i) {
        if (menuBar->getMenuTitle(i) == L"Window") {
            windowMenu = menuBar->getMenu(i);
            break;
        }
    }
    CHECK_NOT_NULL(windowMenu);
    if (windowMenu != nullptr) {
        MenuItem* reopen = windowMenu->getItem(0);
        CHECK_NOT_NULL(reopen);
        if (reopen != nullptr) {
            CHECK(reopen->getText() == L"Render Settings");
            CHECK(reopen->handleClick());
        }
    }

    CHECK(render->isVisible());
    CHECK(dock->findCard("card_render") == render);
    CHECK(session.ui().findById("card_render") == render);
    CHECK(render->getContent() == renderContent);
    CHECK(dynamic_cast<DockTabGroup*>(render->getParent()) != nullptr);

    struct PersistentPanelCase {
        const char* id;
        size_t menuItem;
    };
    const PersistentPanelCase panels[] = {
        {"card_render", 0},
        {"card_inspector", 1},
        {"card_outliner", 2},
        {"card_network", 3},
        {"card_console", 4},
        {"card_assets", 5},
    };
    for (const auto& panel : panels) {
        auto* card = dynamic_cast<DockCard*>(session.ui().findById(panel.id));
        CHECK_NOT_NULL(card);
        if (card == nullptr || windowMenu == nullptr) continue;
        Widget* const content = card->getContent();
        MenuItem* const reopen = windowMenu->getItem(panel.menuItem);
        CHECK_NOT_NULL(reopen);
        if (reopen == nullptr) continue;

        for (int cycle = 0; cycle < 8; ++cycle) {
            CHECK(dock->requestCloseCard(card));
            CHECK(dock->findCard(panel.id) == card);
            CHECK(session.ui().findById(panel.id) == card);
            CHECK(card->getContent() == content);
            CHECK(reopen->handleClick());
            CHECK(dock->findCard(panel.id) == card);
            CHECK(card->getContent() == content);
            CHECK(dynamic_cast<DockTabGroup*>(card->getParent()) != nullptr);
        }
    }

    session.shutdown();
}

TEST_CASE(test_editor_session_play_mode_viewport_is_game_surface) {
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Play);

    auto* viewport = dynamic_cast<Image*>(session.ui().findById("panel_viewport"));
    CHECK(viewport != nullptr);

    const float centerX =
        (viewport->getWorldBounds().minX + viewport->getWorldBounds().maxX) * 0.5f;
    const float centerY =
        (viewport->getWorldBounds().minY + viewport->getWorldBounds().maxY) * 0.5f;

    CHECK(!session.onMouseMove(centerX, centerY));
    CHECK(session.onMouseWheel(centerX, centerY, 1.0f));
    CHECK(session.getUiCursorHint() == UiCursorHint::Default);

    session.shutdown();
}

TEST_CASE(editor_game_view_rejects_play_when_host_bootstrap_fails)
{
    // A session without a native host window cannot initialize presentation.
    // The transport transition must be transactional: failed bootstrap leaves
    // the editor in Edit instead of presenting a false Play state.
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, ""));

    CHECK_FALSE(session.gameView().trySetMode(EditorMode::Play));
    CHECK(session.gameView().mode() == EditorMode::Edit);

    session.shutdown();
}

TEST_CASE(editor_focus_loss_clears_shortcut_modifiers)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.onKeyDown(UIKey_Control);
    session.onWindowFocusChanged(false);
    CHECK_FALSE(session.onKeyDown(UIKey_Z));
    session.shutdown();
}

TEST_CASE(editor_viewport_click_commits_text_focus)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* input = dynamic_cast<TextInput*>(
        session.ui().findById("transform_px"));
    auto* viewport = dynamic_cast<Image*>(
        session.ui().findById("panel_viewport"));
    CHECK(input != nullptr);
    CHECK(viewport != nullptr);
    if (input == nullptr || viewport == nullptr) {
        session.shutdown();
        return;
    }

    session.ui().setFocus(input);
    CHECK(session.ui().getFocusedWidget() == input);
    const auto bounds = viewport->getWorldBounds();
    const float x = (bounds.minX + bounds.maxX) * 0.5f;
    const float y = (bounds.minY + bounds.maxY) * 0.5f;
    CHECK(session.onMouseButtonDown(x, y, 0));
    CHECK(session.ui().getFocusedWidget() == nullptr);
    session.onMouseButtonUp(x, y, 0);

    session.shutdown();
}

TEST_CASE(editor_viewport_first_click_recovers_stale_ui_capture_after_arrow_key)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* input = dynamic_cast<TextInput*>(
        session.ui().findById("transform_px"));
    auto* viewport = dynamic_cast<Image*>(
        session.ui().findById("panel_viewport"));
    CHECK(input != nullptr);
    CHECK(viewport != nullptr);
    if (input == nullptr || viewport == nullptr) {
        session.shutdown();
        return;
    }

    const auto inputBounds = input->getWorldBounds();
    const auto inputCenter = session.ui().logicalToPhysical({
        (inputBounds.minX + inputBounds.maxX) * 0.5f,
        (inputBounds.minY + inputBounds.maxY) * 0.5f});
    const float inputX = inputCenter.x;
    const float inputY = inputCenter.y;
    CHECK(session.onMouseButtonDown(inputX, inputY, 0));
    CHECK(session.ui().isCapturing());

    session.onKeyDown(UIKey_Right);
    session.onKeyUp(UIKey_Right);

    const auto viewportBounds = viewport->getWorldBounds();
    const auto viewportCenter = session.ui().logicalToPhysical({
        (viewportBounds.minX + viewportBounds.maxX) * 0.5f,
        (viewportBounds.minY + viewportBounds.maxY) * 0.5f});
    const float viewportX = viewportCenter.x;
    const float viewportY = viewportCenter.y;
    CHECK(session.onMouseButtonDown(viewportX, viewportY, 0));
    CHECK_FALSE(session.ui().isCapturing());
    CHECK(session.onMouseButtonUp(viewportX, viewportY, 0));

    session.shutdown();
}

TEST_CASE(editor_viewport_wheel_converts_ui_pixels_and_anchors_at_pointer)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* viewport = dynamic_cast<Image*>(
        session.ui().findById("panel_viewport"));
    CHECK(viewport != nullptr);
    if (viewport == nullptr) {
        session.shutdown();
        return;
    }

    const auto bounds = viewport->getWorldBounds();
    const float x = bounds.maxX - bounds.width() * 0.15f;
    const float y = bounds.minY + bounds.height() * 0.35f;
    const ayt::math::FVector3 initialEye = session.freecam().eye();

    // One native wheel notch reaches EditorSession as -40 AYUI pixels.
    CHECK(session.onMouseWheel(x, y, -40.0f));
    const ayt::math::FVector3 delta = session.freecam().eye() - initialEye;
    CHECK_FLOAT_EQ(delta.lengthSq(), 0.5625f, 1.0e-4f);
    // Off-centre cursor anchoring must include a lateral component; a legacy
    // forward-only dolly has zero projection onto screen-right.
    CHECK(delta.dot(session.freecam().right()) > 0.1f);

    session.shutdown();
}

TEST_CASE(editor_viewport_rmb_owns_freecam_look_and_lmb_does_not)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* viewport = dynamic_cast<Image*>(
        session.ui().findById("panel_viewport"));
    CHECK_NOT_NULL(viewport);
    if (viewport == nullptr) {
        session.shutdown();
        return;
    }

    const auto bounds = viewport->getWorldBounds();
    const float x = (bounds.minX + bounds.maxX) * 0.5f;
    const float y = (bounds.minY + bounds.maxY) * 0.5f;

    CHECK(session.onMouseButtonDown(x, y, 1));
    CHECK(session.freecam().isLooking());
    CHECK(session.onMouseMove(x + 24.0f, y + 12.0f));
    CHECK(session.onMouseButtonUp(x + 24.0f, y + 12.0f, 1));
    CHECK_FALSE(session.freecam().isLooking());

    CHECK(session.onMouseButtonDown(x, y, 0));
    CHECK(session.onMouseMove(x + 24.0f, y + 12.0f));
    CHECK_FALSE(session.freecam().isLooking());
    CHECK(session.onMouseButtonUp(x + 24.0f, y + 12.0f, 0));

    session.shutdown();
}

TEST_CASE(editor_viewport_click_selects_active_world_entity_and_clears_outline)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    ayt::entity::World* world =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    auto* viewport = dynamic_cast<Image*>(
        session.ui().findById("panel_viewport"));
    CHECK(world != nullptr);
    CHECK(viewport != nullptr);
    if (world == nullptr || viewport == nullptr) {
        session.shutdown();
        return;
    }

    ayt::entity::Entity* entity = world->createEntity();
    CHECK(entity != nullptr);
    if (entity == nullptr) {
        session.shutdown();
        return;
    }
    entity->setName("Viewport Pick Target");
    entity->addComponent<ayt::entity::Transform>();
    auto* mesh = entity->addComponent<ayt::entity::MeshComponent>();

    const auto bounds = viewport->getWorldBounds();
    const float centerX = (bounds.minX + bounds.maxX) * 0.5f;
    const float centerY = (bounds.minY + bounds.maxY) * 0.5f;
    CHECK(session.onMouseButtonDown(centerX, centerY, 0));
    CHECK(session.onMouseButtonUp(centerX, centerY, 0));
    CHECK(session.selectedEntityId() == entity->getId());
    CHECK(mesh != nullptr);
    if (mesh != nullptr) CHECK(mesh->outlineHull);

    const float emptyX = bounds.minX + 2.0f;
    const float emptyY = bounds.minY + 2.0f;
    CHECK(session.onMouseButtonDown(emptyX, emptyY, 0));
    CHECK(session.onMouseButtonUp(emptyX, emptyY, 0));
    CHECK(session.selectedEntityId() == 0u);
    if (mesh != nullptr) CHECK_FALSE(mesh->outlineHull);

    session.shutdown();
}

TEST_CASE(editor_viewport_click_selects_entity_in_play_world)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* scenes = ayt::app::currentEngineHost()->scenes();
    ayt::entity::World* editWorld =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    CHECK(scenes != nullptr);
    CHECK(editWorld != nullptr);
    if (scenes == nullptr || editWorld == nullptr) {
        session.shutdown();
        return;
    }

    ayt::entity::Entity* source = editWorld->createEntity();
    CHECK(source != nullptr);
    if (source == nullptr) {
        session.shutdown();
        return;
    }
    source->setName("Play Viewport Pick Target");
    source->addComponent<ayt::entity::Transform>();
    source->addComponent<ayt::entity::MeshComponent>();

    CHECK(scenes->beginPlay());
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Play);
    ayt::entity::World* playWorld =
        session.worldContext().world(EditorWorldSlot::Play, true);
    ayt::entity::Entity* clone = playWorld != nullptr
        ? playWorld->findEntity("Play Viewport Pick Target") : nullptr;
    auto* viewport = dynamic_cast<Image*>(
        session.ui().findById("panel_viewport"));
    CHECK(playWorld != nullptr);
    CHECK(clone != nullptr);
    CHECK(viewport != nullptr);
    if (playWorld != nullptr && clone != nullptr && viewport != nullptr) {
        const auto bounds = viewport->getWorldBounds();
        const float x = (bounds.minX + bounds.maxX) * 0.5f;
        const float y = (bounds.minY + bounds.maxY) * 0.5f;
        CHECK(session.onMouseButtonDown(x, y, 0));
        CHECK(session.onMouseButtonUp(x, y, 0));
        CHECK(session.selectedEntityId() == clone->getId());
        auto* mesh = clone->getComponent<ayt::entity::MeshComponent>();
        CHECK(mesh != nullptr);
        if (mesh != nullptr) CHECK(mesh->outlineHull);
        auto* hint = dynamic_cast<TextLabel*>(
            session.ui().findById("inspector_hint"));
        CHECK(hint != nullptr);
        if (hint != nullptr) {
            CHECK(hint->getText().rfind(L"Play selection: ", 0) == 0);
        }
        auto* positionX = dynamic_cast<TextInput*>(
            session.ui().findById("transform_px"));
        CHECK(positionX != nullptr);
        if (positionX != nullptr) {
            CHECK(positionX->getText() != L"-");
            CHECK(positionX->isReadOnly());
        }
    }

    scenes->endPlay();
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Edit);
    session.shutdown();
}

TEST_CASE(editor_undo_redo_menu_items_follow_edit_mode)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* menuBar = dynamic_cast<MenuBar*>(session.ui().findById("menubar"));
    CHECK(menuBar != nullptr);
    MenuItem* undo = nullptr;
    MenuItem* redo = nullptr;
    if (menuBar != nullptr) {
        for (size_t menuIndex = 0; menuIndex < menuBar->getMenuCount(); ++menuIndex) {
            Menu* menu = menuBar->getMenu(menuIndex);
            if (menu == nullptr) continue;
            for (size_t itemIndex = 0; itemIndex < menu->getItemCount(); ++itemIndex) {
                MenuItem* item = menu->getItem(itemIndex);
                if (item == nullptr) continue;
                if (item->getText() == L"Undo") undo = item;
                if (item->getText() == L"Redo") redo = item;
            }
        }
    }
    CHECK(undo != nullptr);
    CHECK(redo != nullptr);
    if (undo == nullptr || redo == nullptr) {
        session.shutdown();
        return;
    }

    CHECK(undo->isEnabled());
    CHECK(redo->isEnabled());
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Play);
    CHECK(!undo->isEnabled());
    CHECK(!redo->isEnabled());
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Edit);
    CHECK(undo->isEnabled());
    CHECK(redo->isEnabled());

    session.shutdown();
}

TEST_CASE(editor_view_menu_toggles_viewport_orientation_axis)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    bool persistedVisible = true;
    int persistenceCalls = 0;
    EditorSessionDesc desc{};
    desc.uiBackend = &backend;
    desc.layoutPath = layoutPath;
    desc.viewportOrientationAxisVisible = true;
    desc.onViewportOrientationAxisVisibilityChanged =
        [&](bool visible) {
            persistedVisible = visible;
            ++persistenceCalls;
        };
    CHECK(session.initialize(desc));

    auto* menuBar = dynamic_cast<MenuBar*>(session.ui().findById("menubar"));
    CHECK(menuBar != nullptr);
    MenuItem* axisItem = nullptr;
    if (menuBar != nullptr) {
        for (size_t menuIndex = 0; menuIndex < menuBar->getMenuCount(); ++menuIndex) {
            if (menuBar->getMenuTitle(menuIndex) != L"View") continue;
            Menu* viewMenu = menuBar->getMenu(menuIndex);
            if (viewMenu != nullptr && viewMenu->getItemCount() != 0) {
                axisItem = viewMenu->getItem(0);
            }
            break;
        }
    }

    CHECK(axisItem != nullptr);
    CHECK(session.viewportOrientationAxisVisible());
    if (axisItem != nullptr) {
        CHECK(axisItem->getText() == L"[x] Viewport Orientation Axis");
        CHECK(axisItem->handleClick());
        CHECK_FALSE(session.viewportOrientationAxisVisible());
        CHECK_FALSE(persistedVisible);
        CHECK(persistenceCalls == 1);
        CHECK(axisItem->getText() == L"[ ] Viewport Orientation Axis");
        CHECK(axisItem->handleClick());
        CHECK(session.viewportOrientationAxisVisible());
        CHECK(persistedVisible);
        CHECK(persistenceCalls == 2);
    }

    session.shutdown();
}

// === PR-5 (v0.1.2 LM-2) =====================================================
//
// 设计依据（design v0.1.1 §7 LM-2）：
//   * Play/Paused 时锁 Inspector 写路径
//   * 4 button click handler (pickInspectorSkel/Anim + applyInspectorOverrides +
//     resetInspectorOverrides) + commitInspectorOverrides helper 入口守卫
//   * inspector_hint 文案切换：Edit "Click buttons to configure." /
//     Play/Paused "Locked during Play."
//
// 验证策略：
//   * EditorSession 实例化 + 拉起到能调 onModeChanged 的状态（layout 加载后）
//   * Edit 模式：4 click handler 不 no-op（_allowInspectorEdit == true）
//   * 切 Play：handler 早返；inspector_hint 文案 = "Locked during Play"
//   * 切回 Edit：恢复
//   * 本 case 不依赖真实 bgfx — 用现有 mock fixture + 加载 editor_shell.ui.json

#include <string>

TEST_CASE(editor_inspector_locked_during_play_LM2)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSessionDesc desc{};
    desc.uiBackend = &backend;
    desc.layoutPath = layoutPath;

    EditorSession session;
    CHECK(session.initialize(desc));

    // 默认模式 = Edit — verify 4 button click handler 不被锁。
    // 间接验证：通过访问 gameView().mode() 确认 EditorMode 切换语义；
    // 守卫字段是 private，没有 public accessor；走 inspector_hint 文案
    // 间接观察（hint 文案仅当 _allowInspectorEdit 切时同步切）。
    CHECK(session.gameView().mode() == EditorMode::Edit);

    // Edit 初始 hint = "Click buttons to configure."（ui.json 默认）
    if (auto* w = session.ui().findById("inspector_hint")) {
        if (auto* lbl = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            // 既有文本验证（hint 已加载）
            CHECK(!lbl->getText().empty());
        }
    }

    // 切 Play → onModeChanged 应当 fire；hint 文案应当切到 "Locked during Play."
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Play);
    if (auto* w = session.ui().findById("inspector_hint")) {
        if (auto* lbl = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            CHECK(lbl->getText() == std::wstring(L"Locked during Play."));
        }
    }

    // 切回 Edit → hint 恢复。
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Edit);
    if (auto* w = session.ui().findById("inspector_hint")) {
        if (auto* lbl = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            CHECK(lbl->getText() == std::wstring(L"Click buttons to configure."));
        }
    }

    // 切 Paused → 也锁。
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Paused);
    if (auto* w = session.ui().findById("inspector_hint")) {
        if (auto* lbl = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            CHECK(lbl->getText() == std::wstring(L"Locked during Play."));
        }
    }

    session.shutdown();
}

TEST_CASE(editor_preferences_restore_workspace_camera_tool_and_render_state)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorPreferences requested;
    requested.viewportOrientationAxisVisible = false;
    requested.panelNetworkVisible = true;
    requested.cameraPoseValid = true;
    requested.cameraEye = {1.0f, 2.0f, 3.0f};
    requested.cameraYawRadians = 0.35f;
    requested.cameraPitchRadians = -0.2f;
    requested.cameraMoveSpeed = 9.0f;
    requested.activeTool = EditorTool::Move;
    requested.localTransformSpace = true;
    requested.themeName = kAliyatEditorDarkTheme;
    requested.density = EditorDensity::Compact;
    requested.uiScale = 0.90f;
    requested.gamma = 2.0f;
    requested.bloomEnabled = false;

    EditorPreferences persisted;
    int persistenceCalls = 0;
    EditorSessionDesc desc{};
    desc.uiBackend = &backend;
    desc.layoutPath = layoutPath;
    desc.viewportOrientationAxisVisible =
        requested.viewportOrientationAxisVisible;
    desc.preferences = requested;
    desc.onPreferencesChanged =
        [&](const EditorPreferences& value) {
            persisted = value;
            ++persistenceCalls;
        };

    EditorSession session;
    CHECK(session.initialize(desc));
    CHECK(session.activeTool() == EditorTool::Move);
    CHECK_FALSE(session.viewportOrientationAxisVisible());
    CHECK_FLOAT_EQ(session.freecam().eye().x, 1.0f, 1.0e-5f);
    CHECK_FLOAT_EQ(session.freecam().eye().y, 2.0f, 1.0e-5f);
    CHECK_FLOAT_EQ(session.freecam().eye().z, 3.0f, 1.0e-5f);
    CHECK_FLOAT_EQ(session.freecam().yawRadians(), 0.35f, 1.0e-5f);
    CHECK_FLOAT_EQ(session.freecam().pitchRadians(), -0.2f, 1.0e-5f);
    CHECK_FLOAT_EQ(session.freecam().moveSpeed(), 9.0f, 1.0e-5f);
    CHECK_FLOAT_EQ(session.ui().getUiScale(), 0.90f, 1.0e-5f);
    CHECK(ThemeManager::get().getActiveThemeName() == kAliyatEditorDarkTheme);

    auto* network = dynamic_cast<DockCard*>(
        session.ui().findById("card_network"));
    auto* toolLabel = dynamic_cast<TextLabel*>(
        session.ui().findById("lbl_active_tool"));
    auto* gamma = dynamic_cast<Slider*>(session.ui().findById("sld_gamma"));
    auto* bloom = dynamic_cast<CheckBox*>(session.ui().findById("chk_bloom"));
    CHECK(network != nullptr);
    CHECK(toolLabel != nullptr);
    CHECK(gamma != nullptr);
    CHECK(bloom != nullptr);
    if (network != nullptr) CHECK(network->isVisible());
    if (toolLabel != nullptr) CHECK(toolLabel->getText() == L"Universal");
    if (gamma != nullptr) CHECK_FLOAT_EQ(gamma->getValue(), 2.0f, 1.0e-5f);
    if (bloom != nullptr) CHECK_FALSE(bloom->isChecked());

    session.savePreferencesNow();
    CHECK(persistenceCalls == 1);
    CHECK(persisted.activeTool == EditorTool::Move);
    CHECK(persisted.localTransformSpace);
    CHECK(persisted.panelNetworkVisible);
    CHECK_FALSE(persisted.bloomEnabled);
    CHECK(persisted.themeName == kAliyatEditorDarkTheme);
    CHECK(persisted.density == EditorDensity::Compact);
    CHECK_FLOAT_EQ(persisted.uiScale, 0.90f, 1.0e-5f);
    CHECK(!persisted.dockTree.empty());

    CHECK(session.ui().findById("btn_tool_select") == nullptr);
    CHECK(session.ui().findById("btn_tool_move") == nullptr);
    CHECK(session.ui().findById("btn_tool_rotate") == nullptr);
    CHECK(session.ui().findById("btn_tool_scale") == nullptr);

    session.shutdown();
}

TEST_CASE(editor_compact_visual_profile_is_applied)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* toolbar = session.ui().findById("workspace_toolbar");
    auto* universal = dynamic_cast<TextLabel*>(
        session.ui().findById("lbl_active_tool"));
    auto* documentTitle = dynamic_cast<TextLabel*>(
        session.ui().findById("lbl_document_title"));
    auto* renderHeader = dynamic_cast<TextLabel*>(
        session.ui().findById("lbl_render_hdr"));
    auto* inspector = dynamic_cast<DockCard*>(
        session.ui().findById("card_inspector"));

    CHECK_FLOAT_EQ(session.ui().getUiScale(), 1.0f, 1.0e-5f);
    CHECK(ThemeManager::get().getActiveThemeName() == kAliyatEditorDarkTheme);
    CHECK_NOT_NULL(toolbar);
    CHECK_NOT_NULL(universal);
    CHECK_NOT_NULL(documentTitle);
    CHECK_NOT_NULL(renderHeader);
    CHECK_NOT_NULL(inspector);
    if (toolbar != nullptr) CHECK_FLOAT_EQ(toolbar->getHeight(), 34.0f, 1.0e-5f);
    if (universal != nullptr) {
        CHECK(universal->getText() == L"Universal");
        CHECK_FLOAT_EQ(universal->getWidth(), 76.0f, 1.0e-5f);
    }
    if (documentTitle != nullptr) CHECK(documentTitle->getFontSize() == 12);
    if (renderHeader != nullptr) CHECK(renderHeader->getFontSize() == 13);
    if (inspector != nullptr) {
        CHECK_FLOAT_EQ(inspector->getHeaderHeight(), 20.0f, 1.0e-5f);
    }

    session.shutdown();
}

TEST_CASE(editor_scene_uses_one_universal_gizmo_without_mode_buttons)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    CHECK(session.ui().findById("btn_tool_select") == nullptr);
    CHECK(session.ui().findById("btn_tool_move") == nullptr);
    CHECK(session.ui().findById("btn_tool_rotate") == nullptr);
    CHECK(session.ui().findById("btn_tool_scale") == nullptr);
    auto* universal = dynamic_cast<TextLabel*>(
        session.ui().findById("lbl_active_tool"));
    CHECK_NOT_NULL(universal);
    if (universal != nullptr) CHECK(universal->getText() == L"Universal");
    for (int cycle = 0; cycle < 32; ++cycle) {
        session.update(1.0f / 240.0f);
        session.render();
    }

    session.shutdown();
}

TEST_CASE(editor_space_button_click_recovers_lost_viewport_mouse_up)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    MockRenderer backend;
    EditorSessionDesc desc{};
    desc.uiBackend = &backend;
    desc.layoutPath = layoutPath;
    desc.preferences.activeTool = EditorTool::Scale;

    EditorSession session;
    CHECK(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);

    auto* viewport = dynamic_cast<Image*>(session.ui().findById("panel_viewport"));
    auto* space = dynamic_cast<Button*>(session.ui().findById("btn_tool_space"));
    CHECK_NOT_NULL(viewport);
    CHECK_NOT_NULL(space);
    if (viewport == nullptr || space == nullptr) {
        session.shutdown();
        return;
    }

    const auto viewportBounds = viewport->getWorldBounds();
    const float viewportX = (viewportBounds.minX + viewportBounds.maxX) * 0.5f;
    const float viewportY = (viewportBounds.minY + viewportBounds.maxY) * 0.5f;
    CHECK(session.onMouseButtonDown(viewportX, viewportY, 0));

    // Deliberately omit the viewport mouse-up. The next toolbar press must
    // recover both the viewport gesture and AYUI capture, then deliver its
    // own matching mouse-up to the World/Local button.
    CHECK(clickSessionButton(session, space));
    CHECK(space->getText() == L"Local");
    CHECK_FALSE(session.freecam().isLooking());
    CHECK_FALSE(session.ui().isCapturing());

    session.shutdown();
}

TEST_CASE(editor_transform_inspector_writes_edit_entity_and_supports_undo)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    ayt::entity::World* world =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    auto* viewport = dynamic_cast<Image*>(
        session.ui().findById("panel_viewport"));
    CHECK(world != nullptr);
    CHECK(viewport != nullptr);
    if (world == nullptr || viewport == nullptr) {
        session.shutdown();
        return;
    }

    ayt::entity::Entity* entity = world->createEntity();
    CHECK(entity != nullptr);
    if (entity == nullptr) {
        session.shutdown();
        return;
    }
    entity->setName("Editable Transform");
    auto* transform = entity->addComponent<ayt::entity::Transform>();
    entity->addComponent<ayt::entity::MeshComponent>();
    CHECK(transform != nullptr);

    const auto bounds = viewport->getWorldBounds();
    const float x = (bounds.minX + bounds.maxX) * 0.5f;
    const float y = (bounds.minY + bounds.maxY) * 0.5f;
    CHECK(session.onMouseButtonDown(x, y, 0));
    CHECK(session.onMouseButtonUp(x, y, 0));
    CHECK(session.selectedEntityId() == entity->getId());

    auto* positionX = dynamic_cast<TextInput*>(
        session.ui().findById("transform_px"));
    auto* apply = dynamic_cast<Button*>(
        session.ui().findById("btn_transform_apply"));
    CHECK(positionX != nullptr);
    CHECK(apply != nullptr);
    if (positionX != nullptr && apply != nullptr && transform != nullptr) {
        CHECK_FALSE(positionX->isReadOnly());
        positionX->setText(L"2.500");
        CHECK(clickButton(apply));
        CHECK_FLOAT_EQ(transform->position.x, 2.5f, 1.0e-5f);
        CHECK(session.document() != nullptr);
        if (session.document() != nullptr) CHECK(session.document()->isDirty());

        session.onKeyDown(UIKey_Control);
        CHECK(session.onKeyDown(UIKey_Z));
        session.onKeyUp(UIKey_Control);
        CHECK_FLOAT_EQ(transform->position.x, 0.0f, 1.0e-5f);
    }

    session.shutdown();
}

TEST_CASE(editor_universal_gizmo_requires_handle_instead_of_object_surface_drag)
{
    const std::string layoutPath = resolveEditorShellLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    ayt::entity::World* world =
        session.worldContext().world(EditorWorldSlot::Edit, true);
    auto* viewport = dynamic_cast<Image*>(
        session.ui().findById("panel_viewport"));
    CHECK(world != nullptr);
    CHECK(viewport != nullptr);
    if (world == nullptr || viewport == nullptr) {
        session.shutdown();
        return;
    }

    ayt::entity::Entity* entity = world->createEntity();
    CHECK(entity != nullptr);
    if (entity == nullptr) {
        session.shutdown();
        return;
    }
    entity->setName("Move Target");
    auto* transform = entity->addComponent<ayt::entity::Transform>();
    entity->addComponent<ayt::entity::MeshComponent>();
    CHECK(transform != nullptr);
    if (transform == nullptr) {
        session.shutdown();
        return;
    }
    const ayt::math::FVector3 before = transform->position;
    const auto bounds = viewport->getWorldBounds();
    const float x = (bounds.minX + bounds.maxX) * 0.5f;
    const float y = (bounds.minY + bounds.maxY) * 0.5f;
    // A click selects the object first. A subsequent drag beginning on the
    // object surface is inert rather than camera navigation; transforms are
    // changed only after an explicit Universal Gizmo handle is hit.
    CHECK(session.onMouseButtonDown(x, y, 0));
    CHECK(session.onMouseButtonUp(x, y, 0));
    CHECK(session.selectedEntityId() == entity->getId());
    session.onMouseMove(x, y);
    CHECK(session.getUiCursorHint() == UiCursorHint::Default);

    CHECK(session.onMouseButtonDown(x, y, 0));
    CHECK(session.getUiCursorHint() == UiCursorHint::Default);
    CHECK(session.onMouseMove(x + 96.0f, y));
    CHECK(session.onMouseButtonUp(x + 96.0f, y, 0));
    CHECK_FALSE(session.freecam().isLooking());
    CHECK(session.selectedEntityId() == entity->getId());
    CHECK_FLOAT_EQ(transform->position.x, before.x, 1.0e-5f);
    CHECK_FLOAT_EQ(transform->position.y, before.y, 1.0e-5f);
    CHECK_FLOAT_EQ(transform->position.z, before.z, 1.0e-5f);

    session.onKeyDown(UIKey_Control);
    CHECK_FALSE(session.onKeyDown(UIKey_Z));
    session.onKeyUp(UIKey_Control);
    CHECK_FLOAT_EQ(transform->position.x, before.x, 1.0e-5f);
    CHECK_FLOAT_EQ(transform->position.y, before.y, 1.0e-5f);
    CHECK_FLOAT_EQ(transform->position.z, before.z, 1.0e-5f);

    session.shutdown();
}

TEST_SUITE_END
