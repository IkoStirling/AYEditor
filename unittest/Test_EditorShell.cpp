#include "AYTest.h"
#include "AYEditorSession.h"
#include "AYMockRenderer.h"
#include "AYBox.h"
#include "AYSplitterHandle.h"
#include "AYButton.h"
#include "AYImage.h"
#include "AYWindow.h"

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
                "id": "icon_toolbar",
                "size": { "h": 28 },
                "children": [
                    { "type": "Button", "id": "btn_icon_1", "text": "T1" },
                    { "type": "TextLabel", "id": "lbl_mode", "text": "EDIT" }
                ]
            }
        ]
    })";

    CHECK(ui.loadFromString(json));
    CHECK(ui.findById("menubar") != nullptr);
    CHECK(ui.findById("btn_icon_1") != nullptr);
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
    CHECK(session.ui().findById("btn_icon_1") != nullptr);
    CHECK(session.ui().findById("btn_close") != nullptr);
    CHECK(session.ui().findById("panel_viewport") != nullptr);
    CHECK(session.ui().findById("panel_inspector") != nullptr);
    session.shutdown();
}

// ED-03: the inspector panel must render the Phase-3 widget
// IDs after the shell JSON loads. assert no id is missing
// (otherwise the user clicks an inspector button that is not
// bound). Manual verify only for the actual pick / apply flow
// (Win32 file dialogs block inside CI).
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
        return; // same skip convention as the other layout-driven tests.
    }

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    // Inspector read-only labels (4)
    CHECK_NOT_NULL(session.ui().findById("inspector_hint"));
    CHECK_NOT_NULL(session.ui().findById("inspector_mesh"));
    CHECK_NOT_NULL(session.ui().findById("inspector_skel"));
    CHECK_NOT_NULL(session.ui().findById("inspector_anim"));

    // Inspector action buttons (4). Cast to Button* to confirm
    // the JSON parsed them as Buttons (vs TextLabels).
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

TEST_CASE(test_editor_session_play_mode_split_capture) {
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

    auto* split =
        dynamic_cast<SplitterHandle*>(session.ui().findById("split_hierarchy_viewport"));
    auto* hierarchy = dynamic_cast<Window*>(session.ui().findById("panel_hierarchy"));
    auto* viewport = dynamic_cast<Image*>(session.ui().findById("panel_viewport"));
    CHECK(split != nullptr);
    CHECK(hierarchy != nullptr);
    CHECK(viewport != nullptr);

    const float splitX = (split->getWorldBounds().minX + split->getWorldBounds().maxX) * 0.5f;
    const float splitY = split->getWorldBounds().minY + 20.0f;

    CHECK(session.onMouseButtonDown(splitX, splitY, 0));
    CHECK(session.ui().isCapturing());
    CHECK(split->isDragging());

    session.gameView().setMode(EditorMode::Play);
    CHECK(!session.ui().isCapturing());
    CHECK(!split->isDragging());

    CHECK(session.onMouseButtonDown(splitX, splitY, 0));
    CHECK(split->isDragging());

    const float centerX =
        (viewport->getWorldBounds().minX + viewport->getWorldBounds().maxX) * 0.5f;
    const float centerY =
        (viewport->getWorldBounds().minY + viewport->getWorldBounds().maxY) * 0.5f;

    CHECK(session.onMouseMove(centerX, centerY));
    CHECK(session.onMouseButtonUp(centerX, centerY, 0));
    CHECK(!split->isDragging());
    CHECK(!session.ui().isCapturing());
    CHECK(session.getUiCursorHint() == UiCursorHint::Default);

    session.shutdown();
}

TEST_CASE(test_editor_session_play_mode_clears_splitter_hover_in_viewport) {
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

    auto* split =
        dynamic_cast<SplitterHandle*>(session.ui().findById("split_hierarchy_viewport"));
    auto* viewport = dynamic_cast<Image*>(session.ui().findById("panel_viewport"));
    CHECK(split != nullptr);
    CHECK(viewport != nullptr);

    const float splitX = (split->getWorldBounds().minX + split->getWorldBounds().maxX) * 0.5f;
    const float splitY = (split->getWorldBounds().minY + split->getWorldBounds().maxY) * 0.5f;
    const float centerX =
        (viewport->getWorldBounds().minX + viewport->getWorldBounds().maxX) * 0.5f;
    const float centerY =
        (viewport->getWorldBounds().minY + viewport->getWorldBounds().maxY) * 0.5f;

    CHECK(session.onMouseMove(splitX, splitY));
    CHECK(session.getUiCursorHint() == UiCursorHint::SizeHorizontal);

    CHECK(!session.onMouseMove(centerX, centerY));
    CHECK(session.getUiCursorHint() == UiCursorHint::Default);

    session.shutdown();
}

TEST_SUITE_END