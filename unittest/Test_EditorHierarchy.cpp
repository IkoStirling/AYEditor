// Test_EditorHierarchy.cpp — v0.3+ PR-5 Hierarchy / Outliner entity tree
//
// design §4.3.y — Hierarchy 面板 + 决策 1b（mode-keyed World 源）
//
// 覆盖维度：
//   1. card_outliner + tree_outliner 在 layout 加载后存在
//   2. node 数 == 合成 root(1) + 当前 World entity 数（INV-4 gate：
//      !edit->isDirty() && !sm->isEditDirty()）
//   3. 点击行 → Inspector 目标切到该 entity（"Entity: <name>"）
//   4. mode 切换 → Outliner 重建（延迟，经 update(dt) 消费）
//   5. root 折叠/展开 → 逻辑 entity 选择保持并恢复行高亮
//   6. 无 host/Edit World → tree 空 + hint "Scene: -"
//
// 不覆盖（deferred to e2e / manual）：
//   - 真层级（Entity 无 parent 字段，AYEntity/EntityImpl.h:69-73）
//   - 滚动（TreeView 自测覆盖，AYUI C-12）
//   - Outliner 多选 / 搜索 / 过滤（Outliner v1 仅显示 + 单选）
//
// Landmines（PR-5 review 时检查）：
//   - fileExists / layoutFileExists 已占 → 本文件用 hierarchyLayoutFileExists
//   - 主 TU 合并撞名 → 同上
//   - 不能在 main.cpp + CMakeLists.txt 双注册（Test_EditorTransportDirtyPrompt.cpp 已 ship 双注册 landmine）→
//     本文件**只**通过 main.cpp `#include` 注册，不进 add_executable

#include "AYTest.h"
#include "AYEditor/EditorSession.h"
#include "EditorGameViewTestAccess.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/Box.h"
#include "AYUI/Button.h"
#include "AYUI/TextLabel.h"
#include "AYUI/TreeView.h"
#include "AYUI/DockCard.h"
#include "AYUI/DockArea.h"
#include "AYUI/Menu.h"
#include "AYUI/MenuBar.h"
#include "AYUI/MenuItem.h"
#include "AYUI/UIKeyCode.h"
#include <AYEntity/components/SpriteComponent.h>
#include <AYEntity/components/TilemapComponent.h>
#include <AYEntity/components/OrthoCameraComponent.h>

#include "AYScene.h"
#include "AYScene/SceneManager.h"
#include "AYScene/SceneMode.h"
#include "AYApplication/IEngineHost.h"
#include "AYApplication.h"
#include "AYEntity.h"

#include <sys/stat.h>
#include <string>
#if defined(_WIN32)
#include "AYDevice/WindowManager.h"
#include <Windows.h>
#endif

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

// Product assets have one superproject-owned source anchor; tests must not
// depend on their temporary working directory or a copied build-tree layout.
std::string resolveHierarchyLayoutPath()
{
    const std::string candidates[] = {
        AY_EDITOR_TEST_SOURCE_DIR "/ui/editor_shell.ui.json",
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
    CHECK(!layoutPath.empty());
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
    CHECK(!layoutPath.empty());
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

// case 3: 点击行 → Inspector 切到该 entity（"Entity: <name>"）
TEST_CASE(editor_hierarchy_click_selects_entity_for_inspector)
{
    auto layoutPath = resolveHierarchyLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    // Play → Hierarchy 源切到 EditorWorldContext Play slot。
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Play);
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
    CHECK(hint->getText().rfind(L"Play entity: ", 0) == 0);

    // flat 0 = 合成 root → 清选择，Inspector 退回 PR-4 路径
    tree->setSelectedIndex(0);
    auto* hint2 = dynamic_cast<TextLabel*>(
        session.ui().findById("inspector_hint"));
    CHECK_NOT_NULL(hint2);
    CHECK(hint2->getText().rfind(L"Play entity: ", 0) != 0);

    session.shutdown();
}

// case 4: mode 切换 → Outliner 重建（延迟，经 update(dt) 消费）
TEST_CASE(editor_hierarchy_refresh_on_mode_change)
{
    auto layoutPath = resolveHierarchyLayoutPath();
    CHECK(!layoutPath.empty());
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
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Play);
    session.update(0.016f);                      // 消费 _outlinerRefreshPending
    CHECK(hint->getText() != editHint);
    CHECK(tree->getNodeCount() == 0);

    // Edit → 回到原 hint + 合成 root
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Edit);
    session.update(0.016f);
    CHECK(hint->getText() == editHint);
    CHECK(tree->getNodeCount() == editNodes);

    session.shutdown();
}

// case 5: AYUI TreeView 的可见 flat index 会随折叠改变。AYEditor 必须用
// entity id 保留逻辑选择，并在再次展开后恢复正确的视觉行高亮。
TEST_CASE(editor_hierarchy_root_collapse_preserves_entity_selection)
{
    const std::string layoutPath = resolveHierarchyLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* sm = ayt::app::currentEngineHost()->scenes();
    auto* tree = dynamic_cast<TreeView*>(
        session.ui().findById("tree_outliner"));
    auto* inspectorHint = dynamic_cast<TextLabel*>(
        session.ui().findById("inspector_hint"));
    CHECK_NOT_NULL(sm);
    CHECK_NOT_NULL(tree);
    CHECK_NOT_NULL(inspectorHint);
    if (sm == nullptr || sm->edit() == nullptr || tree == nullptr
        || inspectorHint == nullptr) {
        session.shutdown();
        return;
    }

    ayt::entity::Entity* entity = sm->edit()->world().createEntity();
    CHECK_NOT_NULL(entity);
    if (entity == nullptr) {
        session.shutdown();
        return;
    }
    entity->setName("TreeView Selection Sentinel");

    // Direct World mutation intentionally does not dirty Scene; use the same
    // deferred refresh boundary as a mode/world-source notification.
    EditorGameViewTestAccess::forceModeAndNotify(
        session.gameView(), EditorMode::Edit);
    session.update(0.016f);

    int entityFlatIndex = -1;
    for (size_t i = 1; i < tree->getNodeCount(); ++i) {
        if (tree->getNodeData(i).label == L"TreeView Selection Sentinel") {
            entityFlatIndex = static_cast<int>(i);
            break;
        }
    }
    CHECK(entityFlatIndex > 0);
    if (entityFlatIndex <= 0) {
        session.shutdown();
        return;
    }

    tree->setSelectedIndex(entityFlatIndex);
    const std::wstring selectedHint = inspectorHint->getText();
    CHECK(selectedHint.rfind(L"Entity: ", 0) == 0);

    tree->toggleExpand(0);
    session.update(0.016f);
    CHECK(tree->getNodeCount() == 1u);
    CHECK(tree->getSelectedIndex() == -1);
    CHECK(inspectorHint->getText() == selectedHint);

    tree->toggleExpand(0);
    session.update(0.016f);
    CHECK(tree->getNodeCount() > 1u);
    CHECK(tree->getSelectedIndex() == entityFlatIndex);
    CHECK(inspectorHint->getText() == selectedHint);

    session.shutdown();
}

// case 6: 无 host → tree 空 + hint "Scene: -"
TEST_CASE(editor_hierarchy_empty_when_no_edit_world)
{
    auto layoutPath = resolveHierarchyLayoutPath();
    CHECK(!layoutPath.empty());
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

// Regression: Editor installs a host callback after Menu::addItem().  The
// Menu's own close callback must survive that assignment, otherwise Create
// mutates the World but leaves an active popup intercepting all later input.
TEST_CASE(editor_create_empty_entity_menu_closes_and_refreshes_hierarchy)
{
    const std::string layoutPath = resolveHierarchyLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));

    auto* sm = ayt::app::currentEngineHost()->scenes();
    auto* menuBar = dynamic_cast<MenuBar*>(session.ui().findById("menubar"));
    auto* tree = dynamic_cast<TreeView*>(
        session.ui().findById("tree_outliner"));
    CHECK_NOT_NULL(sm);
    CHECK_NOT_NULL(menuBar);
    CHECK_NOT_NULL(tree);
    if (sm == nullptr || sm->edit() == nullptr || menuBar == nullptr
        || tree == nullptr) {
        session.shutdown();
        return;
    }

    Menu* editMenu = nullptr;
    MenuItem* createItem = nullptr;
    for (size_t menuIndex = 0; menuIndex < menuBar->getMenuCount(); ++menuIndex) {
        Menu* menu = menuBar->getMenu(menuIndex);
        if (menu == nullptr) continue;
        for (size_t itemIndex = 0; itemIndex < menu->getItemCount(); ++itemIndex) {
            MenuItem* item = menu->getItem(static_cast<int>(itemIndex));
            if (item != nullptr && item->getText() == L"Create Empty Entity") {
                editMenu = menu;
                createItem = item;
                break;
            }
        }
    }
    CHECK_NOT_NULL(editMenu);
    CHECK_NOT_NULL(createItem);
    if (editMenu == nullptr || createItem == nullptr) {
        session.shutdown();
        return;
    }

    const size_t before = sm->edit()->world().getAllEntities().size();
    editMenu->open(menuBar, ayt::math::FVector2(0.0f, 26.0f));
    CHECK(editMenu->isOpen());
    CHECK(createItem->handleClick());
    CHECK_FALSE(editMenu->isOpen());
    CHECK(sm->edit()->world().getAllEntities().size() == before + 1u);

    session.update(0.016f);
    CHECK(tree->getNodeCount() == before + 2u); // scene root + entities

    session.shutdown();
}

// Regression: the editor's five fixed-width menu anchors used to overflow the
// 240-DIP MenuBar slot.  They still painted, but the flexible spacer (a later
// HBox sibling) won hit testing over Tools/Help, leaving only a hairline of the
// visible buttons clickable.  Drive the real EditorSession input path so this
// also covers viewport/chrome routing and popup mounting.
TEST_CASE(editor_top_menu_anchors_and_popup_rows_are_fully_hittable)
{
    const std::string layoutPath = resolveHierarchyLayoutPath();
    CHECK(!layoutPath.empty());
    if (layoutPath.empty()) return;

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, layoutPath));
    session.setClientSize(1280.0f, 720.0f);

    auto* menuBar = dynamic_cast<MenuBar*>(session.ui().findById("menubar"));
    CHECK_NOT_NULL(menuBar);
    if (menuBar == nullptr) {
        session.shutdown();
        return;
    }

    std::vector<Button*> anchors;
    for (Widget* child : menuBar->getChildren()) {
        if (auto* button = dynamic_cast<Button*>(child)) {
            anchors.push_back(button);
        }
    }
    CHECK(anchors.size() == menuBar->getMenuCount());

    for (size_t i = 0; i < anchors.size(); ++i) {
        Button* anchor = anchors[i];
        const ayt::math::FRectangle bounds = anchor->getWorldBounds();
        const float x = (bounds.minX + bounds.maxX) * 0.5f;
        const float y = (bounds.minY + bounds.maxY) * 0.5f;
        session.onMouseMove(x, y);
        CHECK(anchor->isMouseOver());
        CHECK(session.onMouseButtonDown(x, y, 0));
        CHECK(session.onMouseButtonUp(x, y, 0));
        CHECK(menuBar->getMenu(i)->isOpen());
        menuBar->closeOpenMenu();
    }

    // A popup row extends below the top bar and over the central viewport.
    // It must still receive the real session-level move/down/up sequence.
    Menu* helpMenu = menuBar->getMenu(menuBar->getMenuCount() - 1u);
    MenuItem* about = helpMenu != nullptr ? helpMenu->getItem(0) : nullptr;
    CHECK_NOT_NULL(helpMenu);
    CHECK_NOT_NULL(about);
    if (helpMenu != nullptr && about != nullptr) {
        Button* helpAnchor = anchors.back();
        const ayt::math::FRectangle anchorBounds = helpAnchor->getWorldBounds();
        const float anchorX = (anchorBounds.minX + anchorBounds.maxX) * 0.5f;
        const float anchorY = (anchorBounds.minY + anchorBounds.maxY) * 0.5f;
        session.onMouseButtonDown(anchorX, anchorY, 0);
        session.onMouseButtonUp(anchorX, anchorY, 0);
        session.update(0.016f);

        const ayt::math::FRectangle itemBounds = about->getWorldBounds();
        const float itemX = (itemBounds.minX + itemBounds.maxX) * 0.5f;
        const float itemY = (itemBounds.minY + itemBounds.maxY) * 0.5f;
        CHECK(session.onMouseMove(itemX, itemY));
        CHECK(about->isMouseOver());
        CHECK(session.onMouseButtonDown(itemX, itemY, 0));
        CHECK(session.onMouseButtonUp(itemX, itemY, 0));
        CHECK_FALSE(helpMenu->isOpen());
    }

    session.shutdown();
}

TEST_CASE(hierarchy_create_button_creates_all_types_and_undoes_scene_edits)
{
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, resolveHierarchyLayoutPath()));
    session.setClientSize(1280.0f, 720.0f);
    auto* button = dynamic_cast<Button*>(session.ui().findById("btn_outliner_create"));
    auto* menu = dynamic_cast<Menu*>(session.ui().findById("outliner_create_menu"));
    auto* tree = dynamic_cast<TreeView*>(session.ui().findById("tree_outliner"));
    CHECK_NOT_NULL(button);
    CHECK_NOT_NULL(menu);
    CHECK_NOT_NULL(tree);
    if (!button || !menu || !tree) { session.shutdown(); return; }
    CHECK(button->isEnabled());
    CHECK(menu->getItemCount() == 4u);
    auto& world = session.document()->scene().world();
    const size_t before = world.getAllEntities().size();

    for (size_t type = 0; type < 4u; ++type) {
        tree->toggleExpand(0); // Creation must reveal a previously collapsed tree.
        const auto bounds = button->getWorldBounds();
        const auto pos = session.ui().logicalToPhysical(
            {(bounds.minX + bounds.maxX) * 0.5f, (bounds.minY + bounds.maxY) * 0.5f});
        session.onMouseMove(pos.x, pos.y);
        CHECK(session.onMouseButtonDown(pos.x, pos.y, 0));
        CHECK(session.onMouseButtonUp(pos.x, pos.y, 0));
        CHECK(menu->isOpen());
        session.update(0.2f);
        const auto row = menu->getItem(type)->getWorldBounds();
        const auto pick = session.ui().logicalToPhysical(
            {(row.minX + row.maxX) * 0.5f, (row.minY + row.maxY) * 0.5f});
        session.onMouseMove(pick.x, pick.y);
        CHECK(session.onMouseButtonDown(pick.x, pick.y, 0));
        CHECK(session.onMouseButtonUp(pick.x, pick.y, 0));
        CHECK_FALSE(menu->isOpen());
        session.update(0.2f);
        CHECK(world.getAllEntities().size() == before + type + 1u);
        CHECK(tree->getNodeCount() == before + type + 2u);
        auto* entity = world.findEntity(session.selectedEntityId());
        CHECK_NOT_NULL(entity);
        if (entity) {
            CHECK(entity->getComponent<ayt::entity::Transform>() != nullptr);
            if (type == 1u) CHECK(entity->getComponent<ayt::entity::SpriteComponent>() != nullptr);
            if (type == 2u) CHECK(entity->getComponent<ayt::entity::TilemapComponent>() != nullptr);
            if (type == 3u) CHECK(entity->getComponent<ayt::entity::OrthoCameraComponent>() != nullptr);
        }
        CHECK(tree->getSelectedIndex() > 0);
    }
    session.ui().setFocus(nullptr);
    session.onKeyDown(UIKey_Control);
    CHECK(session.onKeyDown(UIKey_Z));
    session.onKeyUp(UIKey_Control);
    CHECK(world.getAllEntities().size() == before + 3u);
    session.onKeyDown(UIKey_Control);
    session.onKeyDown(UIKey_Shift);
    CHECK(session.onKeyDown(UIKey_Z));
    session.onKeyUp(UIKey_Shift);
    session.onKeyUp(UIKey_Control);
    CHECK(world.getAllEntities().size() == before + 4u);
    session.shutdown();
}

TEST_CASE(hierarchy_context_menu_respects_scale_popups_and_play_readonly)
{
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    MockRenderer backend;
    EditorSession session;
    CHECK(session.initialize(&backend, resolveHierarchyLayoutPath()));
    session.ui().setDpiScale(1.5f);
    session.setClientSize(1920.0f, 1080.0f);
    auto* button = dynamic_cast<Button*>(session.ui().findById("btn_outliner_create"));
    auto* menu = dynamic_cast<Menu*>(session.ui().findById("outliner_create_menu"));
    auto* tree = dynamic_cast<TreeView*>(session.ui().findById("tree_outliner"));
    CHECK_NOT_NULL(button);
    CHECK_NOT_NULL(menu);
    CHECK_NOT_NULL(tree);
    if (!button || !menu || !tree) { session.shutdown(); return; }
    const auto treeBounds = tree->getWorldBounds();
    const auto pos = session.ui().logicalToPhysical(
        {treeBounds.minX + 20.0f, treeBounds.minY + 12.0f});
    CHECK(session.onMouseButtonDown(pos.x, pos.y, 1));
    CHECK_FALSE(menu->isOpen());
    CHECK(session.onMouseButtonUp(pos.x, pos.y, 1));
    CHECK(menu->isOpen());
    session.update(0.2f);
    // An open popup above the tree must receive its own right click, rather
    // than opening the underlying tree's context menu again.
    const auto menuBounds = menu->getWorldBounds();
    const auto covered = session.ui().logicalToPhysical(
        {menuBounds.minX + 12.0f, menuBounds.minY + 12.0f});
    session.onMouseButtonDown(covered.x, covered.y, 1);
    session.onMouseButtonUp(covered.x, covered.y, 1);
    CHECK(menu->isOpen());
    CHECK(menu->getWorldBounds().minX == menuBounds.minX);
    CHECK(menu->getWorldBounds().minY == menuBounds.minY);
    const auto outside = session.ui().logicalToPhysical(
        {treeBounds.maxX - 10.0f, treeBounds.maxY - 10.0f});
    session.onMouseButtonDown(outside.x, outside.y, 0);
    session.onMouseButtonUp(outside.x, outside.y, 0);
    CHECK_FALSE(menu->isOpen());
    session.update(0.2f);

    const size_t before = session.document()->scene().world().getAllEntities().size();
    for (const auto mode : {EditorMode::Play, EditorMode::Paused}) {
        EditorGameViewTestAccess::forceModeAndNotify(session.gameView(), mode);
        session.update(0.016f);
        CHECK_FALSE(button->isEnabled());
        for (size_t i = 0; i < menu->getItemCount(); ++i) {
            CHECK_FALSE(menu->getItem(i)->isEnabled());
            const auto row = menu->getItem(i)->getWorldBounds();
            CHECK_FALSE(menu->getItem(i)->onMouseButtonUp(UIMouseEvent(
                {(row.minX + row.maxX) * 0.5f, (row.minY + row.maxY) * 0.5f}, 0)));
        }
        CHECK(session.document()->scene().world().getAllEntities().size() == before);
    }
    EditorGameViewTestAccess::forceModeAndNotify(session.gameView(), EditorMode::Edit);
    session.update(0.016f);
    CHECK(button->isEnabled());
    // Leave the popup mounted to exercise session-owned overlay teardown.
    CHECK(session.onMouseButtonDown(pos.x, pos.y, 1));
    CHECK(session.onMouseButtonUp(pos.x, pos.y, 1));
    CHECK(menu->isOpen());
    session.shutdown();
}

#if defined(_WIN32)
TEST_CASE(hierarchy_creation_follows_detached_panel_and_survives_close)
{
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::device::WindowManager windowManager;
    ayt::device::WindowCreateInfo windowInfo{};
    windowInfo.title = "AYEditor hierarchy creation test";
    windowInfo.width = 1280;
    windowInfo.height = 720;
    windowInfo.hidden = true;
    CHECK(windowManager.createWindow(windowInfo));
    if (windowManager.getWindowHandle() == nullptr) return;
    MockRenderer backend;
    EditorSession session;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = resolveHierarchyLayoutPath();
    desc.hostWindow = windowManager.getWindowHandle();
    desc.childWindowManager = &windowManager;
    CHECK(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);
    auto* dock = dynamic_cast<DockArea*>(session.ui().findById("main_dock"));
    auto* card = dynamic_cast<DockCard*>(session.ui().findById("card_outliner"));
    auto* menu = dynamic_cast<Menu*>(session.ui().findById("outliner_create_menu"));
    CHECK_NOT_NULL(dock);
    CHECK_NOT_NULL(card);
    CHECK_NOT_NULL(menu);
    if (!dock || !card || !menu) {
        session.shutdown(); windowManager.destroyWindow(); return;
    }
    CHECK(dock->floatCard("card_outliner", {100.0f, 120.0f}));
    {
        auto active = UIManager::pushActive(&session.ui());
        auto* create = dynamic_cast<Button*>(session.ui().findById("btn_outliner_create"));
        CHECK_NOT_NULL(create);
        if (create) {
            const auto bounds = create->getWorldBounds();
            const UIMouseEvent event({(bounds.minX + bounds.maxX) * 0.5f,
                (bounds.minY + bounds.maxY) * 0.5f}, 0);
            CHECK(create->onMouseButtonDown(event));
            CHECK(create->onMouseButtonUp(event));
        }
        CHECK(menu->isOpen());
    }
    CHECK(card->detachToOwnWindow());
    auto* children = session.childWindows();
    CHECK(children->count() == 1u);
    if (children->count() != 1u) {
        session.shutdown(); windowManager.destroyWindow(); return;
    }
    auto& childUi = *children->entries()[0].ui;
    const auto childHandle = children->entries()[0].handle;
    children->tickAll(0.2f);
    auto* button = dynamic_cast<Button*>(childUi.findById("btn_outliner_create"));
    auto* tree = dynamic_cast<TreeView*>(childUi.findById("tree_outliner"));
    CHECK_NOT_NULL(button);
    CHECK_NOT_NULL(tree);
    if (button && tree) {
        const size_t before = session.document()->scene().world().getAllEntities().size();
        {
            auto active = UIManager::pushActive(&childUi);
            const auto bounds = button->getWorldBounds();
            const UIMouseEvent event({(bounds.minX + bounds.maxX) * 0.5f,
                (bounds.minY + bounds.maxY) * 0.5f}, 0);
            CHECK(button->onMouseButtonDown(event));
            CHECK(button->onMouseButtonUp(event));
            CHECK(menu->isOpen());
            CHECK(menu->getParent() == childUi.getOverlayRoot());
            CHECK(menu->getItem(1)->handleClick());
        }
        children->tickAll(0.2f);
        session.update(0.2f);
        CHECK(session.document()->scene().world().getAllEntities().size() == before + 1u);
        CHECK(tree->getSelectedIndex() > 0);
        EditorGameViewTestAccess::forceModeAndNotify(session.gameView(), EditorMode::Play);
        CHECK_FALSE(button->isEnabled());
        EditorGameViewTestAccess::forceModeAndNotify(session.gameView(), EditorMode::Edit);
        CHECK(button->isEnabled());
        const auto bounds = tree->getWorldBounds();
        const auto pos = childUi.logicalToPhysical({bounds.minX + 20.0f, bounds.minY + 12.0f});
        const auto packed = MAKELPARAM(static_cast<int>(pos.x), static_cast<int>(pos.y));
        ::SendMessageW(static_cast<HWND>(childHandle), WM_RBUTTONDOWN, MK_RBUTTON, packed);
        ::SendMessageW(static_cast<HWND>(childHandle), WM_RBUTTONUP, 0, packed);
        CHECK(menu->isOpen());
        CHECK(menu->getParent() == childUi.getOverlayRoot());
    }
    children->closeChildWindow(childHandle);
    CHECK(children->count() == 0u);
    CHECK(session.ui().findById("outliner_create_menu") == menu);
    CHECK_FALSE(menu->isOpen());
    session.shutdown();
    windowManager.destroyWindow();
}
#endif

TEST_SUITE_END
