#include "AYTest.h"

#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/EditorPlayRuntime.h"
#include "AYEditor/EditorSession.h"
#include "AYApplication.h"
#include "AYApplication/IEngineHost.h"
#include "AYProject/Project.h"
#include "AYUI/TileView.h"
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
        AY_EDITOR_TEST_SOURCE_DIR "/ui/editor_shell.ui.json";
    return std::filesystem::exists(path) ? path.string() : std::string{};
}

ayt::math::FVector2 rectCenter(const ayt::math::FRectangle& rectangle)
{
    return ayt::math::FVector2(
        (rectangle.minX + rectangle.maxX) * 0.5f,
        (rectangle.minY + rectangle.maxY) * 0.5f);
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
    CHECK(classifyEditorAssetPath("material.phoskia") == EditorAssetType::Shader);
    CHECK(classifyEditorAssetPath("controller.logia") == EditorAssetType::Script);
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
    CHECK(database.portableAssetPath(*mesh) == "meshes/Hero.aymesh");
    CHECK(database.portableAssetPath(mesh->absolutePath)
          == "meshes/Hero.aymesh");
    const EditorAssetRecord* source =
        database.findByLogicalPath("Assets/Characters/Hero.fbx");
    CHECK(source != nullptr);
    if (source != nullptr) {
        CHECK(database.portableAssetPath(*source)
              == "Characters/Hero.fbx");
    }
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
    auto* list = dynamic_cast<ayt::ui::TileView*>(
        session.ui().findById("list_assets"));
    CHECK(tree != nullptr);
    CHECK(list != nullptr);
    CHECK(session.assetDatabase().records().size() == 1u);
    if (tree == nullptr || list == nullptr) {
        session.shutdown();
        return;
    }

    // Assets root contains Models. Tile selection is independent from folder
    // activation; navigation requires a complete double click.
    CHECK(list->getItemCount() == 1u);
    ayt::ui::TileCell* folderCell = list->cellForLogicalIndex(0);
    CHECK(folderCell != nullptr);
    CHECK(folderCell != nullptr
          && folderCell->getInfoStripText() == L"DIR");
    CHECK(folderCell != nullptr
          && !folderCell->isCornerMarkerVisible());
    const auto folderClick = session.ui().logicalToPhysical(
        folderCell != nullptr
            ? rectCenter(folderCell->getThumbnailRect())
            : ayt::math::FVector2{});
    for (int click = 0; click < 2; ++click) {
        session.onMouseMove(folderClick.x, folderClick.y);
        (void)session.onMouseButtonDown(folderClick.x, folderClick.y, 0);
        (void)session.onMouseButtonUp(folderClick.x, folderClick.y, 0);
        if (click == 0) session.update(0.1f);
    }
    session.update(0.0f);
    CHECK(list->getItemCount() == 1u);
    CHECK(list->getItem(0).find(L"Crate.aymesh") != std::wstring::npos);
    ayt::ui::TileCell* assetCell = list->cellForLogicalIndex(0);
    CHECK(assetCell != nullptr);
    CHECK(assetCell != nullptr
          && assetCell->getInfoStripText() == L"MESH");
    CHECK(assetCell != nullptr
          && assetCell->isCornerMarkerVisible());

    const auto assetClick = session.ui().logicalToPhysical(
        assetCell != nullptr
            ? rectCenter(assetCell->getThumbnailRect())
            : ayt::math::FVector2{});
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

    auto* list = dynamic_cast<ayt::ui::TileView*>(
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
        list->scrollToIndex(targetIndex);
        ayt::ui::TileCell* target = list->cellForLogicalIndex(targetIndex);
        CHECK(target != nullptr);
        const auto click = session.ui().logicalToPhysical(
            target != nullptr ? rectCenter(target->getThumbnailRect())
                              : ayt::math::FVector2{});
        for (int count = 0; count < 2; ++count) {
            session.onMouseMove(click.x, click.y);
            (void)session.onMouseButtonDown(click.x, click.y, 0);
            (void)session.onMouseButtonUp(click.x, click.y, 0);
            if (count == 0) session.update(0.1f);
        }
        session.update(0.0f);
    }
    CHECK(list->getItemCount() == 1u);
    CHECK_FLOAT_EQ(list->getScrollOffset().y, 0.0f, 1e-5f);
    CHECK(list->getItem(0).find(L"Crate.aymesh") != std::wstring::npos);
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

    auto* list = dynamic_cast<ayt::ui::TileView*>(
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
    ayt::ui::TileCell* dragCell = list->cellForLogicalIndex(0);
    CHECK(dragCell != nullptr);
    const auto press = session.ui().logicalToPhysical(
        dragCell != nullptr ? rectCenter(dragCell->getThumbnailRect())
                            : ayt::math::FVector2{});
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
    if (auto* created = scenes->edit()->world().findEntity(
            session.selectedEntityId())) {
        auto* mesh = created->getComponent<ayt::entity::MeshComponent>();
        CHECK(mesh != nullptr);
        if (mesh != nullptr) {
            CHECK(mesh->meshPath == "Crate.aymesh");
            CHECK_FALSE(std::filesystem::path(mesh->meshPath).is_absolute());
        }
    } else {
        CHECK(false);
    }
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
