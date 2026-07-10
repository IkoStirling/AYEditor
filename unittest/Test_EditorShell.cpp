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
    ui.bindEvent("btn_play", "onClick", []() {});

    const char* json = R"({
        "type": "VBox",
        "id": "editor_root",
        "size": { "w": 800, "h": 600 },
        "children": [
            {
                "type": "HBox",
                "id": "toolbar",
                "size": { "h": 40 },
                "children": [
                    { "type": "Button", "id": "btn_play", "text": "Play", "onClick": "play" },
                    { "type": "TextLabel", "id": "lbl_mode", "text": "EDIT" }
                ]
            }
        ]
    })";

    CHECK(ui.loadFromString(json));
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
    CHECK(session.ui().findById("btn_play") != nullptr);
    CHECK(session.ui().findById("panel_viewport") != nullptr);
    CHECK(session.ui().findById("panel_inspector") != nullptr);
    session.shutdown();
}

// Phase 2a: the toolbar Import button must be present after
// loading the shell JSON. We can't drive the Win32 modal dialog
// from a CI unit test (it blocks), so this test only asserts the
// button's presence + that findById returns a Button*. The actual
// ImportDialog::showOpenFileDialog + importCharacterFromDialog
// flow is covered by manual verification per the convention in
// Test_EditorImporter.cpp:9-14 (Win32 UI is never auto-tested).
TEST_CASE(toolbar_btn_import_is_present_after_layout_load) {
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
        // Same convention as test_editor_session_loads_shell_json:
        // skip silently when the asset copy hasn't materialized.
        return;
    }

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* widget = session.ui().findById("btn_import");
    CHECK_NOT_NULL(widget);
    auto* button = dynamic_cast<Button*>(widget);
    CHECK_NOT_NULL(button);

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