#include "AYTest.h"

#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/EditorAssetListView.h"
#include "AYEditor/EditorPlayRuntime.h"
#include "AYEditor/EditorSession.h"
#include "AYApplication.h"
#include "AYApplication/IEngineHost.h"
#include "AYProject/Project.h"
#include "AYScene/SceneManager.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/TextLabel.h"
#include "AYUI/TreeView.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace ayt::editor;

namespace {

std::filesystem::path assetBrowserTempRoot(const char* suffix)
{
    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / (std::string("ayeditor_asset_browser_") + suffix + "_"
           + std::to_string(nonce));
}

void writeAssetBrowserFile(const std::filesystem::path& path,
                           const char* contents = "asset")
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
}

std::string resolveAssetBrowserLayout()
{
    const std::filesystem::path path =
        AY_EDITOR_TEST_SOURCE_DIR "/assets/ui/editor_shell.ui.json";
    return std::filesystem::exists(path) ? path.string() : std::string{};
}

struct AssetBrowserTempCleanup {
    std::filesystem::path root;
    ~AssetBrowserTempCleanup() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

struct ProjectRootRestore {
    std::string previous = ayt::project::Project::instance().root();
    ~ProjectRootRestore() {
        ayt::project::Project::instance().setRoot(previous);
    }
};

} // namespace

TEST_SUITE(AYEditor_AssetBrowser)

TEST_CASE(editor_asset_type_classification_covers_runtime_and_source_files)
{
    CHECK(classifyEditorAssetPath("hero.aymesh") == EditorAssetType::Mesh);
    CHECK(classifyEditorAssetPath("hero.aymat") == EditorAssetType::Material);
    CHECK(classifyEditorAssetPath("walk.ayanm") == EditorAssetType::Animation);
    CHECK(classifyEditorAssetPath("hero.ayskel") == EditorAssetType::Skeleton);
    CHECK(classifyEditorAssetPath("scene.ayscene") == EditorAssetType::Scene);
    CHECK(classifyEditorAssetPath("editor.ui.json") == EditorAssetType::UiLayout);
    CHECK(classifyEditorAssetPath("Hero.FBX") == EditorAssetType::SourceModel);
    CHECK(classifyEditorAssetPath("albedo.PNG") == EditorAssetType::Texture);
}

TEST_CASE(editor_runtime_cache_follows_open_project_root)
{
    ProjectRootRestore restore;
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("cache_root")};
    ayt::project::Project::instance().setRoot(cleanup.root.string());
    std::string actual = EditorPlayRuntime::resolvePersistentCacheRoot();
    while (!actual.empty() && (actual.back() == '/' || actual.back() == '\\')) {
        actual.pop_back();
    }
    const std::filesystem::path expected =
        std::filesystem::absolute(cleanup.root / ".ayeditor_cache")
            .lexically_normal();
    CHECK(std::filesystem::path(actual).lexically_normal() == expected);
}

TEST_CASE(editor_asset_database_scans_filters_searches_and_keeps_stable_ids)
{
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("database")};
    writeAssetBrowserFile(cleanup.root / "Assets/Characters/Hero.fbx");
    writeAssetBrowserFile(cleanup.root / "Assets/Textures/Hero.PNG", "texture");
    writeAssetBrowserFile(
        cleanup.root / ".ayeditor_cache/assets/meshes/Hero.aymesh", "mesh");
    writeAssetBrowserFile(
        cleanup.root / ".ayeditor_cache/assets/meshes/Hero.aydep.json", "{}");

    EditorAssetDatabase database;
    std::string error;
    CHECK(database.open(cleanup.root.string(), &error));
    CHECK(database.scanNow(&error));
    CHECK(error.empty());
    CHECK(database.records().size() == 3u);
    CHECK(database.findByLogicalPath("Assets/Characters/Hero.fbx") != nullptr);
    const EditorAssetRecord* mesh =
        database.findByLogicalPath("Imported/meshes/Hero.aymesh");
    CHECK(mesh != nullptr);
    if (mesh == nullptr) return;
    CHECK(mesh->type == EditorAssetType::Mesh);
    const EditorAssetId stableId = mesh->id;

    const auto direct = database.entries("Imported/meshes");
    CHECK(direct.size() == 1u);
    CHECK(!direct[0].folder);
    CHECK(direct[0].assetId == stableId);
    const auto search = database.entries("Assets", "hero",
                                         EditorAssetType::Texture);
    CHECK(search.size() == 1u);
    CHECK(search[0].type == EditorAssetType::Texture);

    CHECK(database.scanNow(&error));
    mesh = database.findByLogicalPath("Imported/meshes/Hero.aymesh");
    CHECK(mesh != nullptr);
    if (mesh != nullptr) CHECK(mesh->id == stableId);
}

TEST_CASE(editor_asset_browser_layout_selects_asset_and_shows_asset_inspector)
{
    const std::string layout = resolveAssetBrowserLayout();
    CHECK(!layout.empty());
    if (layout.empty()) return;
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("session")};
    writeAssetBrowserFile(cleanup.root / "Assets/Models/Crate.aymesh", "badmesh");

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::ui::MockRenderer renderer;
    EditorSessionDesc desc;
    desc.uiBackend = &renderer;
    desc.layoutPath = layout;
    desc.projectRoot = cleanup.root.string();
    EditorSession session;
    CHECK(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);
    CHECK(session.rescanAssetsNow());

    auto* tree = dynamic_cast<ayt::ui::TreeView*>(
        session.ui().findById("tree_assets"));
    auto* list = dynamic_cast<EditorAssetListView*>(
        session.ui().findById("list_assets"));
    CHECK(tree != nullptr);
    CHECK(list != nullptr);
    CHECK(session.assetDatabase().records().size() == 1u);
    if (tree == nullptr || list == nullptr) {
        session.shutdown();
        return;
    }

    // Assets root contains Models. Folder navigation is intentionally a
    // mouse-up action: an update between down/up must not rebuild the list and
    // reinterpret the release as a click on the new folder's first asset.
    CHECK(list->getItemCount() == 1u);
    const auto rootListBounds = list->getWorldBounds();
    const auto folderClick = session.ui().logicalToPhysical({
        rootListBounds.minX + 24.0f,
        rootListBounds.minY + list->getItemHeight() * 0.5f});
    session.onMouseMove(folderClick.x, folderClick.y);
    (void)session.onMouseButtonDown(folderClick.x, folderClick.y, 0);
    session.update(0.0f);
    CHECK(list->getItem(0).find(L"[Folder]") != std::wstring::npos);
    (void)session.onMouseButtonUp(folderClick.x, folderClick.y, 0);
    session.update(0.0f);
    CHECK(list->getItemCount() == 1u);
    CHECK(list->getItem(0).find(L"[Mesh]") != std::wstring::npos);

    const auto assetListBounds = list->getWorldBounds();
    const auto assetClick = session.ui().logicalToPhysical({
        assetListBounds.minX + 24.0f,
        assetListBounds.minY + list->getItemHeight() * 0.5f});
    session.onMouseMove(assetClick.x, assetClick.y);
    (void)session.onMouseButtonDown(assetClick.x, assetClick.y, 0);
    (void)session.onMouseButtonUp(assetClick.x, assetClick.y, 0);
    CHECK(session.selectedAssetId() != 0);
    auto* assetBody = session.ui().findById("inspector_asset_body");
    auto* entityBody = session.ui().findById("inspector_entity_body");
    auto* typeLabel = dynamic_cast<ayt::ui::TextLabel*>(
        session.ui().findById("inspector_asset_type"));
    CHECK(assetBody != nullptr);
    CHECK(entityBody != nullptr);
    CHECK(typeLabel != nullptr);
    if (assetBody && entityBody && typeLabel) {
        CHECK(assetBody->isVisible());
        CHECK(!entityBody->isVisible());
        CHECK(typeLabel->getText() == L"Type: Mesh");
    }

    // Selecting the asset invalidates the Inspector layout. Composite
    // rendering must consume that layout before temporarily hiding the scene
    // panel; otherwise the VBox drops panel_viewport and the native scene
    // leaks behind the surrounding editor chrome.
    ayt::ui::Widget* viewport = session.ui().findById("panel_viewport");
    CHECK(viewport != nullptr);
    if (viewport != nullptr) {
        const auto beforeComposite = viewport->getWorldBounds();
        session.populateFrame(true);
        const auto duringComposite = viewport->getWorldBounds();
        CHECK_FLOAT_EQ(duringComposite.minX, beforeComposite.minX, 1e-5f);
        CHECK_FLOAT_EQ(duringComposite.minY, beforeComposite.minY, 1e-5f);
        CHECK_FLOAT_EQ(duringComposite.maxX, beforeComposite.maxX, 1e-5f);
        CHECK_FLOAT_EQ(duringComposite.maxY, beforeComposite.maxY, 1e-5f);
        CHECK_FALSE(viewport->isVisible());
        session.flushFrame();
        CHECK(viewport->isVisible());
    }
    session.shutdown();
}

TEST_CASE(editor_asset_browser_folder_navigation_resets_stale_list_scroll)
{
    const std::string layout = resolveAssetBrowserLayout();
    CHECK(!layout.empty());
    if (layout.empty()) return;
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("scroll_reset")};
    for (int i = 0; i < 12; ++i) {
        writeAssetBrowserFile(cleanup.root / "Assets"
            / ("Folder" + std::to_string(i)) / "item.aymesh", "badmesh");
    }
    writeAssetBrowserFile(
        cleanup.root / "Assets/ZZ_Target/Crate.aymesh", "badmesh");

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::ui::MockRenderer renderer;
    EditorSessionDesc desc;
    desc.uiBackend = &renderer;
    desc.layoutPath = layout;
    desc.projectRoot = cleanup.root.string();
    EditorSession session;
    CHECK(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);
    CHECK(session.rescanAssetsNow());

    auto* list = dynamic_cast<EditorAssetListView*>(
        session.ui().findById("list_assets"));
    CHECK(list != nullptr);
    if (list == nullptr) {
        session.shutdown();
        return;
    }
    CHECK(list->getItemCount() == 13u);
    // The mock shell may give the list more vertical space than the real
    // dock. Constrain it so this test genuinely creates a stale offset.
    list->setSize(ayt::math::FVector2(480.0f, 60.0f));
    list->performLayout();
    list->setScrollOffset(ayt::math::FVector2(0.0f, 10000.0f));
    CHECK(list->getScrollOffset().y > 0.0f);

    int targetIndex = -1;
    for (std::size_t i = 0; i < list->getItemCount(); ++i) {
        if (list->getItem(i).find(L"ZZ_Target") != std::wstring::npos) {
            targetIndex = static_cast<int>(i);
            break;
        }
    }
    CHECK(targetIndex >= 0);
    if (targetIndex >= 0) {
        const auto bounds = list->getWorldBounds();
        const float y = bounds.minY
            + static_cast<float>(targetIndex) * list->getItemHeight()
            - list->getScrollOffset().y + list->getItemHeight() * 0.5f;
        const auto click = session.ui().logicalToPhysical({
            bounds.minX + 24.0f, y});
        session.onMouseMove(click.x, click.y);
        (void)session.onMouseButtonDown(click.x, click.y, 0);
        (void)session.onMouseButtonUp(click.x, click.y, 0);
        session.update(0.0f);
    }
    CHECK(list->getItemCount() == 1u);
    CHECK_FLOAT_EQ(list->getScrollOffset().y, 0.0f, 1e-5f);
    CHECK(list->getItem(0).find(L"[Mesh]") != std::wstring::npos);
    session.shutdown();
}

TEST_CASE(editor_asset_drag_reaches_viewport_and_creates_mesh_entity)
{
    const std::string layout = resolveAssetBrowserLayout();
    CHECK(!layout.empty());
    if (layout.empty()) return;
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("drop")};
    writeAssetBrowserFile(cleanup.root / "Assets/Crate.aymesh", "badmesh");

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::ui::MockRenderer renderer;
    EditorSessionDesc desc;
    desc.uiBackend = &renderer;
    desc.layoutPath = layout;
    desc.projectRoot = cleanup.root.string();
    EditorSession session;
    CHECK(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);
    CHECK(session.rescanAssetsNow());

    auto* list = dynamic_cast<EditorAssetListView*>(
        session.ui().findById("list_assets"));
    ayt::ui::Widget* viewport = session.ui().findById("panel_viewport");
    auto* scenes = ayt::app::defaultEngineHost().scenes();
    CHECK(list != nullptr);
    CHECK(viewport != nullptr);
    CHECK(scenes != nullptr);
    CHECK(scenes != nullptr && scenes->edit() != nullptr);
    if (list == nullptr || viewport == nullptr || scenes == nullptr
        || scenes->edit() == nullptr) {
        session.shutdown();
        return;
    }
    const std::size_t before = scenes->edit()->world().getAllEntities().size();
    const auto listBounds = list->getWorldBounds();
    const auto press = session.ui().logicalToPhysical({
        listBounds.minX + 20.0f, listBounds.minY + 10.0f});
    session.onMouseMove(press.x, press.y);
    (void)session.onMouseButtonDown(press.x, press.y, 0);
    CHECK(session.ui().isCapturing());
    auto* assetBody = session.ui().findById("inspector_asset_body");
    auto* entityBody = session.ui().findById("inspector_entity_body");
    CHECK(assetBody != nullptr);
    CHECK(entityBody != nullptr);
    if (assetBody != nullptr && entityBody != nullptr) {
        CHECK(assetBody->isVisible());
        CHECK_FALSE(entityBody->isVisible());
    }

    const auto viewportBounds = viewport->getWorldBounds();
    const auto drop = session.ui().logicalToPhysical({
        (viewportBounds.minX + viewportBounds.maxX) * 0.5f,
        (viewportBounds.minY + viewportBounds.maxY) * 0.5f});
    // Deliberately skip an intermediate move inside the source row. Real
    // touchpads and low-polling mice can jump directly into the viewport;
    // the captured source must still promote that motion into a drag.
    CHECK(session.onMouseMove(drop.x, drop.y));
    CHECK(session.ui().isDragging());
    CHECK(session.onMouseButtonUp(drop.x, drop.y, 0));
    CHECK(!session.ui().isDragging());
    CHECK(scenes->edit()->world().getAllEntities().size() == before + 1u);
    CHECK(session.selectedEntityId() != 0);
    CHECK(session.selectedAssetId() == 0);
    if (assetBody != nullptr && entityBody != nullptr) {
        CHECK_FALSE(assetBody->isVisible());
        CHECK(entityBody->isVisible());
    }

    // Regression: entity -> resource -> the same entity row must switch the
    // Inspector back. Keeping the old Hierarchy highlight meant the second
    // entity click was treated as an unchanged TreeView selection, leaving
    // Reload Asset over the entity controls.
    session.update(0.0f);
    auto* outliner = dynamic_cast<ayt::ui::TreeView*>(
        session.ui().findById("tree_outliner"));
    CHECK(outliner != nullptr);
    CHECK(outliner != nullptr && outliner->getSelectedIndex() > 0);
    list->setSelectedIndex(0);
    CHECK(session.selectedAssetId() != 0);
    CHECK(session.selectedEntityId() == 0);
    CHECK(outliner != nullptr && outliner->getSelectedIndex() == -1);
    if (assetBody != nullptr && entityBody != nullptr) {
        CHECK(assetBody->isVisible());
        CHECK_FALSE(entityBody->isVisible());
    }
    if (outliner != nullptr) outliner->setSelectedIndex(1);
    CHECK(session.selectedAssetId() == 0);
    CHECK(session.selectedEntityId() != 0);
    if (assetBody != nullptr && entityBody != nullptr) {
        CHECK_FALSE(assetBody->isVisible());
        CHECK(entityBody->isVisible());
    }
    CHECK(session.document() != nullptr && session.document()->isDirty());
    session.shutdown();
}

TEST_SUITE_END
