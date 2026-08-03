#include "AYTest.h"
#include "AYEditorSession.h"
#include "AYMockRenderer.h"
#include "AYBox.h"
#include "AYDockArea.h"
#include "AYDockCard.h"
#include "AYButton.h"
#include "AYTextLabel.h"  // PR-5 LM-2 test: hint TextLabel observe
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
    // 找 layout 路径（同 test_editor_session_loads_shell_json 模式）。
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
        // 与 test_editor_session_loads_shell_json 一致：layout 路径不可达
        // (VS Test config working dir 未配) 时静默跳过整 case，不假阳性 fail。
        return;
    }

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
    session.gameView().setMode(EditorMode::Play);
    if (auto* w = session.ui().findById("inspector_hint")) {
        if (auto* lbl = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            CHECK(lbl->getText() == std::wstring(L"Locked during Play."));
        }
    }

    // 切回 Edit → hint 恢复。
    session.gameView().setMode(EditorMode::Edit);
    if (auto* w = session.ui().findById("inspector_hint")) {
        if (auto* lbl = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            CHECK(lbl->getText() == std::wstring(L"Click buttons to configure."));
        }
    }

    // 切 Paused → 也锁。
    session.gameView().setMode(EditorMode::Paused);
    if (auto* w = session.ui().findById("inspector_hint")) {
        if (auto* lbl = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            CHECK(lbl->getText() == std::wstring(L"Locked during Play."));
        }
    }

    session.shutdown();
}

TEST_SUITE_END
