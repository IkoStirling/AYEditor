#include "AYTest.h"
#include "AYEditorSession.h"
#include "AYMockRenderer.h"
#include "AYButton.h"

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

TEST_SUITE_END