#include "AYTest.h"

#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/EditorAssetDeleteAnalysis.h"
#include "AYEditor/EditorShortcutRegistry.h"
#include "AYEditor/EditorPlayRuntime.h"
#include "AYEditor/EditorSession.h"
#include "AYApplication.h"
#include "AYApplication/IEngineHost.h"
#include "AYProject/Project.h"
#include "AYUI/Button.h"
#include "AYUI/Image.h"
#include "AYUI/ModalDialog.h"
#include "AYUI/TileView.h"
#include "AYScene/SceneManager.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/TextLabel.h"
#include "AYUI/TreeView.h"
#include "AYUI/UIKeyCode.h"
#include "../src/EditorAssetPreviewCache.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>

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

std::string resolveAssetBrowserIconRoot()
{
    const std::filesystem::path path =
        std::filesystem::path(AY_EDITOR_TEST_SOURCE_DIR)
        / "../Icons/Tabler";
    return std::filesystem::is_directory(path)
        ? path.lexically_normal().string() : std::string{};
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
    CHECK(classifyEditorAssetPath("ground.aytilemap.json")
          == EditorAssetType::Tilemap);
    CHECK(classifyEditorAssetPath("Hero.FBX") == EditorAssetType::SourceModel);
    CHECK(classifyEditorAssetPath("albedo.PNG") == EditorAssetType::Texture);
    CHECK(classifyEditorAssetPath("material.phoskia") == EditorAssetType::Shader);
    CHECK(classifyEditorAssetPath("controller.logia") == EditorAssetType::Script);
    CHECK(classifyEditorAssetPath("music.ayaudio") == EditorAssetType::Audio);
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
    CHECK(std::filesystem::is_regular_file(database.indexPath()));
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
        CHECK(source->importState == EditorAssetImportState::NeedsImport);
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

TEST_CASE(editor_asset_delete_analysis_reports_text_asset_references)
{
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("delete_analysis")};
    writeAssetBrowserFile(cleanup.root / "Assets/meshes/crate.aymesh", "mesh");
    writeAssetBrowserFile(cleanup.root / "Assets/worlds/demo.ayscene",
        "{\"mesh\":\"meshes/crate.aymesh\"}");

    EditorAssetDatabase database;
    std::string error;
    CHECK(database.open(cleanup.root.string(), &error));
    CHECK(database.scanNow(&error));
    const EditorAssetRecord* target = database.findByLogicalPath(
        "Assets/meshes/crate.aymesh");
    CHECK(target != nullptr);
    if (target != nullptr) {
        const EditorAssetDeleteAnalysis analysis = analyzeEditorAssetDeletion(
            database, {target->id});
        CHECK(analysis.hasExternalReferences());
        CHECK(analysis.references.size() == 1u);
        if (!analysis.references.empty()) {
            CHECK(analysis.references[0].referencingPath
                  == "Assets/worlds/demo.ayscene");
        }
    }
}

TEST_CASE(editor_shortcut_registry_loads_overrides_and_rejects_conflicts)
{
    EditorShortcutRegistry& shortcuts = EditorShortcutRegistry::instance();
    shortcuts.resetToDefaults();
    CHECK(shortcuts.commandFor(ayt::ui::UIKey_F5, 0) == "play.toggle");
    std::string error;
    CHECK(shortcuts.setShortcut("play.toggle", L"Ctrl+P", &error));
    CHECK(shortcuts.commandFor(ayt::ui::UIKey_P, 0x02u) == "play.toggle");
    CHECK_FALSE(shortcuts.setShortcut("play.pause", L"Ctrl+P", &error));
    CHECK(error.find("conflict") != std::string::npos);
}

TEST_CASE(editor_asset_preview_cache_builds_semantic_mesh_thumbnail_once)
{
    int uploads = 0;
    int releases = 0;
    EditorAssetPreviewCache cache(
        [&](std::uint16_t width, std::uint16_t height, const void* pixels) {
            ++uploads;
            CHECK(width == 144u);
            CHECK(height == 96u);
            CHECK(pixels != nullptr);
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5678));
        },
        [&](void* handle) {
            CHECK(handle == reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(0x5678)));
            ++releases;
        });
    EditorAssetRecord record;
    record.absolutePath = "semantic-preview/crate.aymesh";
    record.type = EditorAssetType::Mesh;
    record.size = 42u;
    CHECK(EditorAssetPreviewCache::supports(record));
    CHECK_FALSE(cache.request(record).isValid());
    for (int attempt = 0; attempt < 100 && uploads == 0; ++attempt) {
        (void)cache.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(uploads == 1);
    CHECK(cache.request(record).isValid());
    CHECK(uploads == 1);
    cache.clear();
    CHECK(releases == 1);
}

TEST_CASE(editor_asset_preview_cache_reuses_persisted_thumbnail_after_restart)
{
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("preview_disk")};
    const auto asset = cleanup.root / "Assets/crate.aymesh";
    writeAssetBrowserFile(asset, "bad mesh uses semantic fallback");
    EditorAssetRecord record;
    record.absolutePath = asset.string();
    record.type = EditorAssetType::Mesh;
    record.size = std::filesystem::file_size(asset);
    record.lastModified = static_cast<std::int64_t>(
        std::filesystem::last_write_time(asset).time_since_epoch().count());
    const auto cacheRoot = cleanup.root / ".ayeditor/cache/previews";

    auto populate = [&](EditorAssetPreviewCache& cache, int& uploads) {
        CHECK_FALSE(cache.request(record).isValid());
        for (int attempt = 0; attempt < 200 && uploads == 0; ++attempt) {
            (void)cache.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    int firstUploads = 0;
    {
        EditorAssetPreviewCache cache(
            [&](std::uint16_t, std::uint16_t, const void*) {
                ++firstUploads;
                return reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
            }, [](void*) {});
        cache.setDiskCacheRoot(cacheRoot.string());
        populate(cache, firstUploads);
        CHECK(firstUploads == 1);
        CHECK(cache.diskCacheHitCount() == 0u);
    }
    int secondUploads = 0;
    {
        EditorAssetPreviewCache cache(
            [&](std::uint16_t, std::uint16_t, const void*) {
                ++secondUploads;
                return reinterpret_cast<void*>(static_cast<std::uintptr_t>(2));
            }, [](void*) {});
        cache.setDiskCacheRoot(cacheRoot.string());
        populate(cache, secondUploads);
        CHECK(secondUploads == 1);
        CHECK(cache.diskCacheHitCount() == 1u);
    }
}

TEST_CASE(editor_asset_browser_layout_selects_asset_and_shows_asset_inspector)
{
    const std::string layout = resolveAssetBrowserLayout();
    const std::string iconRoot = resolveAssetBrowserIconRoot();
    CHECK(!layout.empty());
    CHECK(!iconRoot.empty());
    if (layout.empty() || iconRoot.empty()) return;
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("session")};
    writeAssetBrowserFile(cleanup.root / "Assets/Models/Crate.aymesh", "badmesh");

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::ui::MockRenderer renderer;
    EditorSessionDesc desc;
    desc.uiBackend = &renderer;
    desc.layoutPath = layout;
    desc.iconRootPath = iconRoot;
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
    const char* iconButtonIds[] = {
        "btn_assets_add", "btn_assets_up",
        "btn_assets_refresh", "btn_assets_rename", "btn_assets_move",
        "btn_assets_copy", "btn_assets_delete"
    };
    for (const char* id : iconButtonIds) {
        auto* button = dynamic_cast<ayt::ui::Button*>(
            session.ui().findById(id));
        CHECK(button != nullptr);
        CHECK(button != nullptr && button->getText().empty());
        CHECK(button != nullptr && button->getIconDocument() != nullptr);
        CHECK(button != nullptr && !button->getAccessibilityLabel().empty());
    }
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

TEST_CASE(editor_asset_browser_supports_extended_batch_selection_and_confirmed_delete)
{
    const std::string layout = resolveAssetBrowserLayout();
    CHECK(!layout.empty());
    if (layout.empty()) return;
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("batch_delete")};
    const auto first = cleanup.root / "Assets/A.aymesh";
    const auto second = cleanup.root / "Assets/B.aymesh";
    const auto third = cleanup.root / "Assets/C.aymesh";
    writeAssetBrowserFile(first, "mesh-a");
    writeAssetBrowserFile(second, "mesh-b");
    writeAssetBrowserFile(third, "mesh-c");

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
    auto* deleteButton = dynamic_cast<ayt::ui::Button*>(
        session.ui().findById("btn_assets_delete"));
    CHECK(list != nullptr);
    CHECK(deleteButton != nullptr);
    if (list == nullptr || deleteButton == nullptr) {
        session.shutdown();
        return;
    }
    CHECK(list->getSelectionMode()
          == ayt::ui::TileView::SelectionMode::Extended);
    CHECK(list->getItemCount() == 3u);
    list->setSelectedIndices({0, 2});
    CHECK(session.selectedAssetIds().size() == 2u);
    CHECK(deleteButton->isEnabled());

    session.ui().setFocus(list);
    CHECK(session.onKeyDown(ayt::ui::UIKey_Delete));
    CHECK(std::filesystem::is_regular_file(first));
    CHECK(std::filesystem::is_regular_file(third));

    ayt::ui::ModalDialog* confirmation = nullptr;
    if (ayt::ui::Widget* overlay = session.ui().getOverlayRoot()) {
        for (ayt::ui::Widget* child : overlay->getChildren()) {
            if (child != nullptr
                && child->getId() == "asset_delete_confirmation") {
                confirmation = dynamic_cast<ayt::ui::ModalDialog*>(child);
                break;
            }
        }
    }
    CHECK(confirmation != nullptr);
    CHECK(confirmation != nullptr && confirmation->isOpen());
    if (confirmation != nullptr) confirmation->acceptDialog();

    CHECK_FALSE(std::filesystem::exists(first));
    CHECK(std::filesystem::is_regular_file(second));
    CHECK_FALSE(std::filesystem::exists(third));
    CHECK(session.selectedAssetIds().empty());
    CHECK(list->getItemCount() == 1u);
    CHECK_FALSE(deleteButton->isEnabled());
    session.shutdown();
}

TEST_CASE(editor_asset_browser_decodes_png_preview_once_off_the_ui_thread)
{
    const std::string layout = resolveAssetBrowserLayout();
    CHECK(!layout.empty());
    if (layout.empty()) return;
    AssetBrowserTempCleanup cleanup{assetBrowserTempRoot("png_preview")};
    const std::filesystem::path source =
        std::filesystem::path(AY_EDITOR_TEST_SOURCE_DIR)
        / "../../AYRuntime/AYUI/demo/assets/visual_regression/checkerboard.png";
    const std::filesystem::path target =
        cleanup.root / "Assets/checkerboard.png";
    std::error_code copyError;
    std::filesystem::create_directories(target.parent_path(), copyError);
    copyError.clear();
    std::filesystem::copy_file(source, target,
        std::filesystem::copy_options::overwrite_existing, copyError);
    CHECK(copyError.value() == 0);
    CHECK(std::filesystem::is_regular_file(target));
    if (copyError || !std::filesystem::is_regular_file(target)) return;

    int uploadCount = 0;
    int releaseCount = 0;
    std::uint16_t uploadedWidth = 0;
    std::uint16_t uploadedHeight = 0;
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::ui::MockRenderer renderer;
    EditorSessionDesc desc;
    desc.uiBackend = &renderer;
    desc.layoutPath = layout;
    desc.projectRoot = cleanup.root.string();
    desc.createAssetPreviewTexture =
        [&](std::uint16_t width, std::uint16_t height, const void* pixels) {
            ++uploadCount;
            uploadedWidth = width;
            uploadedHeight = height;
            CHECK(pixels != nullptr);
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234));
        };
    desc.releaseAssetPreviewTexture = [&](void* handle) {
        CHECK(handle == reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x1234)));
        ++releaseCount;
    };
    EditorSession session;
    CHECK(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);
    CHECK(session.rescanAssetsNow());

    auto* list = dynamic_cast<ayt::ui::TileView*>(
        session.ui().findById("list_assets"));
    CHECK(list != nullptr);
    for (int attempt = 0; attempt < 200 && uploadCount == 0; ++attempt) {
        session.update(0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(uploadCount == 1);
    CHECK(uploadedWidth > 0u);
    CHECK(uploadedHeight > 0u);
    ayt::ui::TileCell* cell = list != nullptr
        ? list->cellForLogicalIndex(0) : nullptr;
    CHECK(cell != nullptr);
    CHECK(cell != nullptr && cell->getThumbnail().isValid());

    if (list != nullptr) list->setSelectedIndex(0);
    auto* preview = dynamic_cast<ayt::ui::Image*>(
        session.ui().findById("inspector_asset_preview"));
    CHECK(preview != nullptr);
    CHECK(preview != nullptr && preview->isVisible());
    CHECK(preview != nullptr && preview->hasTexture());
    for (int attempt = 0; attempt < 5; ++attempt) session.update(0.0f);
    CHECK(uploadCount == 1);
    session.shutdown();
    CHECK(releaseCount == 1);
}

TEST_SUITE_END
