#include "AYTest.h"
#include "AYEditor/EditorSession.h"
#include "EditorGameViewTestAccess.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/Box.h"
#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "AYUI/Button.h"
#include "AYUI/TextLabel.h"  // PR-5 LM-2 test: hint TextLabel observe
#include "AYUI/Image.h"
#include "AYUI/MenuBar.h"
#include "AYUI/TextInput.h"

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

TEST_SUITE_END
