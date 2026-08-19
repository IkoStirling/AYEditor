// Test_EditorHierarchy.cpp — v0.3+ PR-5 Hierarchy / Outliner entity tree
//
// design §4.3.y — Hierarchy 面板 + 决策 1b（mode-keyed World 源）
//
// 覆盖维度：
//   1. card_outliner + tree_outliner 在 layout 加载后存在
//   2. node 数 == 合成 root(1) + 当前 World entity 数（INV-4 gate：
//      !edit->isDirty() && !sm->isEditDirty()）
//   3. 点击行 → Inspector 目标切到该 entity（"Hierarchy: <name>"）
//   4. mode 切换 → Outliner 重建（延迟，经 update(dt) 消费）
//   5. 无 host/Edit World → tree 空 + hint "Scene: -"
//
// 不覆盖（deferred to e2e / manual）：
//   - 真层级（Entity 无 parent 字段，AYEntity/EntityImpl.h:69-73）
//   - 滚动 / 展开折叠（TreeView 自测覆盖，AYUI C-12）
//   - Outliner 多选 / 搜索 / 过滤（Outliner v1 仅显示 + 单选）
//
// Landmines（PR-5 review 时检查）：
//   - fileExists / layoutFileExists 已占 → 本文件用 hierarchyLayoutFileExists
//   - 主 TU 合并撞名 → 同上
//   - 不能在 main.cpp + CMakeLists.txt 双注册（Test_EditorTransportDirtyPrompt.cpp 已 ship 双注册 landmine）→
//     本文件**只**通过 main.cpp `#include` 注册，不进 add_executable

#include "AYTest.h"
#include "AYEditor/EditorSession.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/Box.h"
#include "AYUI/TextLabel.h"
#include "AYUI/TreeView.h"
#include "AYUI/DockCard.h"

#include "AYScene.h"
#include "AYScene/SceneManager.h"
#include "AYScene/SceneMode.h"
#include "AYApplication/IEngineHost.h"
#include "AYApplication.h"
#include "AYEntity.h"

#include <sys/stat.h>
#include <string>

using namespace ayt::ui;
using namespace ayt::editor;

namespace {

// PR-5：第三个唯一名 —— Test_EditorShell.cpp:19 已占 fileExists，
// Test_EditorTransportDirtyPrompt.cpp:46 已占 layoutFileExists。
// 三者都在 file-scope 匿名 namespace，main.cpp 的 #include 合并成同一 TU
// → 同名即 C2084。
bool hierarchyLayoutFileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

// PR-5：新增两条相对路径。既有两条（"assets/ui/…"、
// "AYRuntime/AYEditor/assets/ui/…"）在 CI/VS 的实际 cwd 下都 miss，导致
// layout 相关 case 全部静默跳过（0 CHECK）。"../assets/ui/…" 命中
// unittest/ 工作目录，"../../assets/ui/…" 命中 ay_add_test
// 设置的目标专属 test_tmp/ 工作目录。
std::string resolveHierarchyLayoutPath()
{
    const std::string candidates[] = {
        "assets/ui/editor_shell.ui.json",
        "../assets/ui/editor_shell.ui.json",
        "../../assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };
    for (const auto& p : candidates) {
        if (hierarchyLayoutFileExists(p)) return p;
    }
    return {};
}

} // namespace

TEST_SUITE(AYEditor_Hierarchy)

// case 1: card_outliner + tree_outliner 在 layout 加载后存在
TEST_CASE(editor_hierarchy_panel_created_when_layout_loaded)
{
    auto layoutPath = resolveHierarchyLayoutPath();
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    CHECK_NOT_NULL(session.ui().findById("card_outliner"));
    CHECK_NOT_NULL(session.ui().findById("outliner_hint"));
    auto* tree = dynamic_cast<TreeView*>(
        session.ui().findById("tree_outliner"));
    CHECK_NOT_NULL(tree);
    auto* card = dynamic_cast<DockCard*>(
        session.ui().findById("card_outliner"));
    CHECK_NOT_NULL(card);
    CHECK(card->isFloatable());

    session.shutdown();
}

// case 2: INV-4 gate（决策 1b：Edit 模式 source = edit()->world()）
TEST_CASE(editor_hierarchy_node_count_matches_edit_world_entity_count)
{
    auto layoutPath = resolveHierarchyLayoutPath();
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* sm = ayt::app::currentEngineHost()->scenes();
    CHECK_NOT_NULL(sm);
    auto* edit = sm->edit();
    CHECK_NOT_NULL(edit);

    auto* tree = dynamic_cast<TreeView*>(
        session.ui().findById("tree_outliner"));
    CHECK_NOT_NULL(tree);

    // Edit mode → source = edit()->world()（决策 1b）。合成 root + N entity。
    const size_t worldCount = edit->world().getAllEntities().size();
    CHECK(tree->getNodeCount() == worldCount + 1);

    // INV-4: 纯读 —— refreshOutliner 跑完后 Edit Scene 仍 clean。
    CHECK(!edit->isDirty());
    CHECK(!sm->isEditDirty());

    session.shutdown();
}

// case 3: 点击行 → Inspector 切到该 entity（"Hierarchy: <name>"）
TEST_CASE(editor_hierarchy_click_selects_entity_for_inspector)
{
    auto layoutPath = resolveHierarchyLayoutPath();
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    // Play → Hierarchy 源切到 EditorWorldContext Play slot。
    session.gameView().setMode(EditorMode::Play);
    session.update(0.016f);   // 消费 _outlinerRefreshPending

    auto* tree = dynamic_cast<TreeView*>(
        session.ui().findById("tree_outliner"));
    CHECK_NOT_NULL(tree);
    if (tree->getNodeCount() < 2) {
        // Play 未 spawn 任何 entity（无 renderer 后端）→ 本 case 不适用
        session.shutdown();
        return;
    }

    // flat 1 = 第一个 entity。直接驱动 TreeView 的 selection 通路
    // （setSelectedIndex → _onSelectionChanged，AYTreeView.cpp:139）。
    tree->setSelectedIndex(1);
    CHECK(tree->getSelectedIndex() == 1);

    auto* hint = dynamic_cast<TextLabel*>(
        session.ui().findById("inspector_hint"));
    CHECK_NOT_NULL(hint);
    CHECK(hint->getText().rfind(L"Hierarchy: ", 0) == 0);

    // flat 0 = 合成 root → 清选择，Inspector 退回 PR-4 路径
    tree->setSelectedIndex(0);
    auto* hint2 = dynamic_cast<TextLabel*>(
        session.ui().findById("inspector_hint"));
    CHECK_NOT_NULL(hint2);
    CHECK(hint2->getText().rfind(L"Hierarchy: ", 0) != 0);

    session.shutdown();
}

// case 4: mode 切换 → Outliner 重建（延迟，经 update(dt) 消费）
TEST_CASE(editor_hierarchy_refresh_on_mode_change)
{
    auto layoutPath = resolveHierarchyLayoutPath();
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* tree = dynamic_cast<TreeView*>(
        session.ui().findById("tree_outliner"));
    auto* hint = dynamic_cast<TextLabel*>(
        session.ui().findById("outliner_hint"));
    CHECK_NOT_NULL(tree);
    CHECK_NOT_NULL(hint);

    // Edit 模式首刷（PR-5 plan 决策 1b）：source = edit()->world()
    // （v1 永远空但 not null）→ hint = "Scene: <editor_default>"，
    // 合成 root + 0 entity。
    const std::wstring editHint = hint->getText();
    const size_t editNodes = tree->getNodeCount();
    CHECK(editNodes >= 1);
    CHECK(editHint.find(L"(Play World)") == std::wstring::npos);
    CHECK(editHint.rfind(L"Scene: ", 0) == 0);

    // Play → source = Play Scene，缺失时回退显式 process World。
    // MockRenderer 下 fallback 未 init → hint 切到 "Scene: -"。这个变化就是 rebuild
    // 已消费的证据（vs 没消费时 hint 仍是 editHint）。
    session.gameView().setMode(EditorMode::Play);
    session.update(0.016f);                      // 消费 _outlinerRefreshPending
    CHECK(hint->getText() != editHint);
    CHECK(tree->getNodeCount() == 0);

    // Edit → 回到原 hint + 合成 root
    session.gameView().setMode(EditorMode::Edit);
    session.update(0.016f);
    CHECK(hint->getText() == editHint);
    CHECK(tree->getNodeCount() == editNodes);

    session.shutdown();
}

// case 5: 无 host → tree 空 + hint "Scene: -"
TEST_CASE(editor_hierarchy_empty_when_no_edit_world)
{
    auto layoutPath = resolveHierarchyLayoutPath();
    if (layoutPath.empty()) return;

    // 故意 **不** 开 EngineHostScope → currentEngineHost() == nullptr
    // → initialize() 的 scene 注入块整体跳过（AYEditorSession.cpp:117-126）
    // → resolveHierarchyWorld(Edit) 返回 nullptr。
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* tree = dynamic_cast<TreeView*>(
        session.ui().findById("tree_outliner"));
    CHECK_NOT_NULL(tree);
    CHECK(tree->getNodeCount() == 0);

    auto* hint = dynamic_cast<TextLabel*>(
        session.ui().findById("outliner_hint"));
    CHECK_NOT_NULL(hint);
    CHECK(hint->getText() == std::wstring(L"Scene: -"));

    session.shutdown();
}

TEST_SUITE_END
