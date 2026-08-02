#include "AYTest.h"
#include "AYEditorSession.h"
#include "AYMockRenderer.h"
#include "AYBox.h"
#include "AYDockArea.h"
#include "AYDockCard.h"
#include "AYButton.h"
#include "AYImage.h"

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
    const std::string candidates[] = {
        "assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };

    std::string layoutPath;
    for (const std::string& path : candidates) {
        if (fileExists(path)) {
            layoutPath = path;
            break;
        }
    }

    if (layoutPath.empty()) {
        return;
    }

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
    session.shutdown();
}

TEST_CASE(inspector_panel_renders_pick_apply_reset_widgets) {
    const std::string candidates[] = {
        "assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };

    std::string layoutPath;
    for (const std::string& path : candidates) {
        if (fileExists(path)) {
            layoutPath = path;
            break;
        }
    }

    if (layoutPath.empty()) {
        return;
    }

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
    const std::string candidates[] = {
        "assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };

    std::string layoutPath;
    for (const std::string& path : candidates) {
        if (fileExists(path)) {
            layoutPath = path;
            break;
        }
    }

    if (layoutPath.empty()) {
        return;
    }

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

TEST_CASE(test_editor_session_play_mode_viewport_is_game_surface) {
    const std::string candidates[] = {
        "assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };

    std::string layoutPath;
    for (const std::string& path : candidates) {
        if (fileExists(path)) {
            layoutPath = path;
            break;
        }
    }

    if (layoutPath.empty()) {
        return;
    }

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);
    session.gameView().setMode(EditorMode::Play);

    auto* viewport = dynamic_cast<Image*>(session.ui().findById("panel_viewport"));
    CHECK(viewport != nullptr);

    const float centerX =
        (viewport->getWorldBounds().minX + viewport->getWorldBounds().maxX) * 0.5f;
    const float centerY =
        (viewport->getWorldBounds().minY + viewport->getWorldBounds().maxY) * 0.5f;

    CHECK(!session.onMouseMove(centerX, centerY));
    CHECK(session.getUiCursorHint() == UiCursorHint::Default);

    session.shutdown();
}

TEST_SUITE_END
