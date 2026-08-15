// Test_EditorTransportDirtyPrompt.cpp — v0.3 PR-4 auto-save-before-play prompt UX
//
// design §4.3.x — Editor 消费 host->scenes() + transport bar UX
//
// 覆盖维度（按 plan §验收闸门）：
//   1. `_editScene` 创建 + setEdit/setCurrent 注入（in EngineHostScope）
//   2. `canBeginPlay()` 反映 _editScene 注入状态（false→true 翻转）
//   3. `lbl_unsaved` 在 dirty 时显 "•"
//   4. `lbl_unsaved` 在 clean 时隐藏
//   5. 跨 mode 切换（Edit → Play） lbl_unsaved 跟随
//
// 不覆盖（deferred to e2e / manual）：
//   - MessageBoxW 拦截（Win32 API，需 mock 库；
//     PR-4 R-6 标注后续 PR 处理）
//   - Save/Discard/Cancel 3 选项 click handler path
//     （依赖 _hostWindow + MessageBoxW；e2e 验证）
//   - auto-save-before-play 弹窗触发 Save 后 edit->save() 路径
//     （依赖 _hostWindow + MessageBoxW）
//
// 测试设计：沿用 Test_EditorShell.cpp 模式（MockRenderer + 真实 layoutPath
// via candidates[]）；并在 EngineHostScope 包裹下注入 _editScene。

#include "AYTest.h"
#include "AYEditor/EditorSession.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/Box.h"
#include "AYUI/Button.h"
#include "AYUI/TextLabel.h"

#include "AYScene/SceneManager.h"
#include "AYScene.h"            // 完整 Scene 定义（clear/isDirty/mode）；forward decl 不够
#include "AYScene/SceneMode.h"
#include "AYApplication/IEngineHost.h"
#include "AYApplication.h"

#include <sys/stat.h>
#include <string>

using namespace ayt::ui;
using namespace ayt::editor;

namespace {

// v0.3 PR-4：改名 layoutFileExists 避免与 Test_EditorShell.cpp 的 fileExists
// 在 main.cpp 的 #include merge 中撞名（C2084）。
bool layoutFileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

std::string resolveLayoutPath()
{
    const std::string candidates[] = {
        "assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };
    for (const auto& p : candidates) {
        if (layoutFileExists(p)) return p;
    }
    return {};
}

} // namespace

TEST_SUITE(AYEditor_TransportDirtyPrompt)

// case 1: _editScene 创建 + host->scenes() 反映
TEST_CASE(editor_transport_edit_scene_injected_after_initialize)
{
    auto layoutPath = resolveLayoutPath();
    if (layoutPath.empty()) {
        // layout 不在 → 跳过（与 Test_EditorShell 同模式）
        return;
    }

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    // _editScene 持有 + SceneMode::Edit
    // 通过 host->scenes()->edit() 观测（PR-3 路径）
    auto* host = ayt::app::currentEngineHost();
    CHECK(host != nullptr);
    auto* sm = host->scenes();
    CHECK(sm != nullptr);

    auto* edit = sm->edit();
    CHECK(edit != nullptr);
    CHECK(edit->mode() == ayt::scene::SceneMode::Edit);

    // 默认 path/load 后 clean → isEditDirty / requireSaveBeforePlay 都 false
    CHECK(!edit->isDirty());
    CHECK(!sm->isEditDirty());
    CHECK(!sm->requireSaveBeforePlay());

    session.shutdown();
}

// case 2: canBeginPlay true 翻转（_edit 注入后）
TEST_CASE(editor_transport_can_begin_play_true_after_initialize)
{
    auto layoutPath = resolveLayoutPath();
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* sm = ayt::app::currentEngineHost()->scenes();
    CHECK(sm != nullptr);
    // initialize() 已 setEdit + setCurrent；canBeginPlay() true
    CHECK(sm->canBeginPlay());

    session.shutdown();
}

// case 3: lbl_unsaved 在 edit dirty 时显 "•"
//   触发方式：mode flip Edit→Play→Edit，onModeChanged 调 refreshUnsavedIndicator。
//   避免 friend accessor 暴露 private 方法。
TEST_CASE(editor_transport_lbl_unsaved_visible_when_edit_dirty)
{
    auto layoutPath = resolveLayoutPath();
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* sm = ayt::app::currentEngineHost()->scenes();
    auto* edit = sm->edit();
    CHECK(edit != nullptr);

    // 初始 clean → lbl_unsaved hidden
    auto* lblInitial = dynamic_cast<ayt::ui::TextLabel*>(
        session.ui().findById("lbl_unsaved"));
    CHECK(lblInitial != nullptr);
    CHECK(!lblInitial->isVisible());

    // edit.clear() 触发 dirty
    edit->clear();
    CHECK(edit->isDirty());
    CHECK(sm->isEditDirty());
    CHECK(sm->requireSaveBeforePlay());

    // mode flip Edit→Play→Edit 触发 onModeChanged → refreshUnsavedIndicator
    session.gameView().setMode(ayt::editor::EditorMode::Play);
    session.gameView().setMode(ayt::editor::EditorMode::Edit);

    auto* lblDirty = dynamic_cast<ayt::ui::TextLabel*>(
        session.ui().findById("lbl_unsaved"));
    CHECK(lblDirty != nullptr);
    CHECK(lblDirty->isVisible());
    CHECK(lblDirty->getText() == L"•");

    session.shutdown();
}

// case 4: lbl_unsaved 在 clean 时隐藏
//   触发方式：mode flip（onModeChanged → refreshUnsavedIndicator）。
TEST_CASE(editor_transport_lbl_unsaved_hidden_when_edit_clean)
{
    auto layoutPath = resolveLayoutPath();
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* sm = ayt::app::currentEngineHost()->scenes();
    auto* edit = sm->edit();
    CHECK(edit != nullptr);

    // 制造 dirty → mode flip → lbl 显
    edit->clear();
    session.gameView().setMode(ayt::editor::EditorMode::Play);
    session.gameView().setMode(ayt::editor::EditorMode::Edit);
    auto* lbl = dynamic_cast<ayt::ui::TextLabel*>(
        session.ui().findById("lbl_unsaved"));
    CHECK(lbl != nullptr);
    CHECK(lbl->isVisible());

    // 替换 _edit 为新 Scene（clean）→ mode flip → lbl 隐
    auto freshEdit = std::make_unique<ayt::scene::Scene>(
        ayt::scene::SceneMode::Edit, "<test_clean_post>");
    sm->setEdit(freshEdit.get());
    sm->setCurrent(freshEdit.get());

    CHECK(!freshEdit->isDirty());
    CHECK(!sm->isEditDirty());
    CHECK(!sm->requireSaveBeforePlay());

    session.gameView().setMode(ayt::editor::EditorMode::Play);
    session.gameView().setMode(ayt::editor::EditorMode::Edit);

    auto* lblClean = dynamic_cast<ayt::ui::TextLabel*>(
        session.ui().findById("lbl_unsaved"));
    CHECK(lblClean != nullptr);
    CHECK(!lblClean->isVisible());
    CHECK(lblClean->getText() == L"");

    // shutdown() reverse：setEdit(nullptr) + setCurrent(nullptr)
    // freshEdit 由 test scope 的 unique_ptr 释放
    session.shutdown();
    (void)edit; // suppress unused warning（已 detach，ptr 安全）
}

// case 5: 跨 mode 切换（Edit → Play）onModeChanged 调 refreshUnsavedIndicator
//   验证：mode 切换后 lbl_unsaved 状态同步（dirty 时仍显 "•"）
TEST_CASE(editor_transport_lbl_unsaved_follows_on_mode_changed)
{
    auto layoutPath = resolveLayoutPath();
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* sm = ayt::app::currentEngineHost()->scenes();
    auto* edit = sm->edit();
    CHECK(edit != nullptr);

    // 制造 dirty（mode 仍 Edit → lbl_unsaved 状态滞后到下次 mode flip 才 refresh）
    edit->clear();

    // 切到 Play → onModeChanged fires → refreshUnsavedIndicator
    session.gameView().setMode(ayt::editor::EditorMode::Play);
    CHECK(session.gameView().mode() == ayt::editor::EditorMode::Play);

    auto* lblAfterMode = dynamic_cast<ayt::ui::TextLabel*>(
        session.ui().findById("lbl_unsaved"));
    CHECK(lblAfterMode != nullptr);
    CHECK(lblAfterMode->isVisible());
    CHECK(lblAfterMode->getText() == L"•");

    // 切回 Edit → onModeChanged 又 fire → refresh（仍 dirty，因为 _edit 没动）
    session.gameView().setMode(ayt::editor::EditorMode::Edit);
    auto* lblAfterEdit = dynamic_cast<ayt::ui::TextLabel*>(
        session.ui().findById("lbl_unsaved"));
    CHECK(lblAfterEdit != nullptr);
    CHECK(lblAfterEdit->isVisible());

    session.shutdown();
}

TEST_SUITE_END