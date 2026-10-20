// Test_EditorImporter.cpp - Phase 1 ED-01 unit tests.
//
// Covers the AYEditor Importer + ImportDialog surface. Goals:
//   * Confirm unsupported extension → clean error result, no crash.
//   * Confirm missing file → "does not exist" error.
//   * Confirm ImportDialog::importFromPath is a one-line pass-through
//     to Importer::importFile (same return shape, same error text).
//
// The lifecycle cases use an authored triangle and an external glTF buffer
// through the real Assimp/AY writers, queue, persisted recipe and asset DB.

#include "AYTest.h"
#include "AYEditor/Importer.h"
#include "AYEditor/EditorAssetImportQueue.h"
#include "AYEditor/ImportDialog.h"
#include "AYEditor/EditorSession.h"
#include <AYUI/MockRenderer.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/TileView.h>
#include "../src/EditorImportRecipeStore.h"
#include "../src/EditorImportSettingsDialog.h"
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include "AYIO/File.h"
#include <AYIO/Path.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <algorithm>

using namespace ayt::editor;

namespace {

// Tiny helper: write `bytes` to `path` (creating parents). We avoid
// AYFile APIs here because the AYIO migration is mid-flight and the
// simple ofstream path is the least surprising one for a unit test.
bool writeStubFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

struct EditorImportFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("ay-editor-import-loop-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ~EditorImportFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::filesystem::path source() const { return root / "External/Triangle.gltf"; }
    std::filesystem::path project() const { return root / "Project"; }
    std::filesystem::path output() const { return project() / ".ayeditor_cache/assets"; }
    void write(const std::filesystem::path& path, const std::string& text) const {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    void triangle(float extent = 1.0f) const {
        const float positions[] = {0, 0, 0, extent, 0, 0, 0, extent, 0};
        write(source().parent_path() / "Triangle.bin", std::string(
            reinterpret_cast<const char*>(positions), sizeof(positions)));
        write(source(), R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Triangle.bin","byteLength":36}],
"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[4,4,0]}],
"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
    }
    ayt::resource::ImportOptions options() const {
        ayt::resource::ImportOptions options;
        options.sourcePath = source().string();
        options.outputDir = output().string();
        options.requireCharacterAssets = false;
        options.loadOption = ayt::resource::IConverter::LoadOption::MeshOnly;
        return options;
    }
    bool finish(EditorAssetImportQueue& queue) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (queue.busy() && std::chrono::steady_clock::now() < deadline) {
            (void)queue.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !queue.busy();
    }
};

} // namespace

TEST_SUITE(AYEditor_Importer)

TEST_CASE(EditorAssetImportQueueReportsFailureAndSupportsRetry)
{
    EditorAssetImportQueue queue;
    const auto id = queue.enqueue(
        "D:/no/such/path/missing.fbx", "D:/tmp/ayeditor-import-queue");
    CHECK(id != 0u);
    for (int attempt = 0; attempt < 200 && queue.busy(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)queue.poll();
    }
    const EditorAssetImportJob* failed = queue.find(id);
    CHECK(failed != nullptr);
    CHECK(failed != nullptr && failed->state == EditorAssetImportJobState::Failed);
    CHECK(failed != nullptr && !failed->message.empty());
    CHECK_FLOAT_EQ(queue.overallProgress(), 1.0f, 1e-5f);

    const auto retryId = queue.retry(id);
    CHECK(retryId != 0u);
    CHECK(retryId != id);
    CHECK(queue.busy());
    while (queue.busy()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)queue.poll();
    }
    const EditorAssetImportJob* retried = queue.find(retryId);
    CHECK(retried != nullptr);
    CHECK(retried != nullptr && retried->force);
    CHECK(retried != nullptr && retried->state == EditorAssetImportJobState::Failed);
}

TEST_CASE(extension_of_lowercases_and_strips_dot)
{
    CHECK_TRUE(Importer::extensionOf("foo.fbx")      == "fbx");
    CHECK_TRUE(Importer::extensionOf("Foo.FBX")      == "fbx");
    CHECK_TRUE(Importer::extensionOf("a/b/c.Png")    == "png");
    CHECK_TRUE(Importer::extensionOf("noext")        == "");
    CHECK_TRUE(Importer::extensionOf("trailing.")    == "");
}

TEST_CASE(is_supported_extension_matches_factory_table)
{
    // Mirror AYResource::isImportSupportedExtension().
    CHECK_TRUE(Importer::isSupportedExtension("a.fbx"));
    CHECK_TRUE(Importer::isSupportedExtension("a.glb"));
    CHECK_TRUE(Importer::isSupportedExtension("a.gltf"));
    CHECK_TRUE(Importer::isSupportedExtension("a.png"));
    CHECK_TRUE(Importer::isSupportedExtension("a.bmp"));
    CHECK_TRUE(Importer::isSupportedExtension("a.tga"));
    CHECK_TRUE(Importer::isSupportedExtension("a.dds"));
    CHECK_TRUE(Importer::isSupportedExtension("a.jpg"));
    CHECK_TRUE(Importer::isSupportedExtension("a.jpeg"));
    CHECK_TRUE(Importer::isSupportedExtension("a.wav"));
    CHECK_FALSE(Importer::isSupportedExtension("a.mp4"));
    CHECK_FALSE(Importer::isSupportedExtension("a"));
}

TEST_CASE(import_empty_sourcepath_returns_error)
{
    Importer::Result r = Importer::importFile("", "C:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(!r.errorMessage.empty());
    CHECK_TRUE(r.errorMessage.find("empty") != std::string::npos);
}

TEST_CASE(import_missing_file_returns_error)
{
    Importer::Result r =
        Importer::importFile("D:/no/such/path/missing.fbx", "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("does not exist") != std::string::npos);
}

TEST_CASE(import_animation_missing_file_returns_error)
{
    Importer::Result r = Importer::importAnimationFile(
        "D:/no/such/path/missing_animation.fbx", "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("does not exist") != std::string::npos);
}

TEST_CASE(import_unsupported_extension_returns_error_with_supported_list)
{
    // Create a real on-disk file with an unsupported extension; the
    // extension check runs *after* the file-exists check.
    const std::string path = (std::filesystem::temp_directory_path()
        / "ayeditor_test_unsupported.mp4").string();
    CHECK_TRUE(writeStubFile(path, "fake video bytes"));

    Importer::Result r = Importer::importFile(path, "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("unsupported") != std::string::npos);
    CHECK_TRUE(r.errorMessage.find("Supported:")   != std::string::npos);

    ayt::io::File::remove(path);
}

TEST_CASE(import_dialog_is_thin_passthrough_to_importer)
{
    // Calling through `ImportDialog::importFromPath` with an empty
    // source must produce the same error shape as a direct call to
    // `Importer::importFile`. The dialog layer is one line; the test
    // pins that contract so a future menu refactor can't silently
    // change the return semantics.
    Importer::Result direct =
        Importer::importFile("", "D:/tmp/cache");
    Importer::Result viaDialog =
        ImportDialog::importFromPath("", "D:/tmp/cache");
    CHECK_FALSE(direct.success);
    CHECK_FALSE(viaDialog.success);
    CHECK_TRUE(direct.errorMessage == viaDialog.errorMessage);
}

TEST_CASE(reimport_queue_recovers_full_persisted_recipe)
{
    EditorImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.materialPolicy.tag = "keep-policy";
    options.materialPolicy.maskNames = "Grate";
    options.sourceCoordinates.tag = "keep-coordinates";
    options.sourceCoordinates.uvOrigin = ayt::resource::ImportUvOrigin::BottomLeft;
    CHECK(Importer::importAssetFile(options).success);
    EditorAssetImportQueue queue;
    const auto id = queue.enqueue(options.sourcePath, options.outputDir, true);
    const auto* queued = queue.find(id);
    CHECK(queued != nullptr);
    if (!queued) return;
    CHECK(queued->options.loadOption == options.loadOption);
    CHECK(queued->options.materialPolicy.maskNames == options.materialPolicy.maskNames);
    CHECK(queued->options.sourceCoordinates.uvOrigin == options.sourceCoordinates.uvOrigin);
    CHECK(fixture.finish(queue));
    CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
    ayt::resource::ImportRecord record;
    CHECK(ayt::resource::readImportRecord((fixture.output() / "Triangle.dep").string(), record));
    CHECK(record.options.materialPolicy.tag == options.materialPolicy.tag);
}

TEST_CASE(import_queue_delivers_immediate_failure_and_background_completion_once)
{
    EditorImportFixture fixture;
    fixture.triangle();
    EditorAssetImportQueue queue;
    const auto valid = queue.enqueue(fixture.options());
    const auto invalid = queue.enqueue("invalid.unsupported", fixture.output().string());
    std::vector<EditorAssetImportJobId> completions;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (completions.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        if (queue.poll()) completions.insert(completions.end(), queue.completedJobIds().begin(),
            queue.completedJobIds().end());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(std::count(completions.begin(), completions.end(), valid) == 1);
    CHECK(std::count(completions.begin(), completions.end(), invalid) == 1);
    CHECK(queue.find(valid)->state == EditorAssetImportJobState::Succeeded);
    CHECK(queue.find(invalid)->state == EditorAssetImportJobState::Failed);
    CHECK_FALSE(queue.poll());
    CHECK(queue.completedJobIds().empty());
}

TEST_CASE(import_database_associates_external_source_and_rechecks_its_dependencies)
{
    EditorImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.sourceCoordinates.tag = "external-recipe";
    CHECK(Importer::importAssetFile(options).success);
    EditorAssetDatabase database;
    CHECK(database.open(fixture.project().string()));
    CHECK(database.scanNow());
    CHECK(database.imports().size() == 1u);
    const auto& imports = database.imports();
    if (imports.empty()) return;
    CHECK(imports[0].state == EditorAssetImportState::Ready);
    const auto resource = imports[0].record.conversion.resources.front().path;
    const auto* mesh = database.findByLogicalPath("Imported/" + resource);
    CHECK(mesh != nullptr);
    if (!mesh) return;
    const auto stableId = mesh->id;
    CHECK(database.importForAsset(stableId) != nullptr);
    CHECK(database.importForAsset(stableId)->record.options.sourceCoordinates.tag == "external-recipe");
    fixture.triangle(2.0f); // External source is deliberately outside both watched asset roots.
    EditorAssetImportQueue queue;
    CHECK(queue.enqueueChangedDependencies(database) == 1u);
    CHECK(fixture.finish(queue));
    CHECK(queue.jobs()[0].state == EditorAssetImportJobState::Succeeded);
    CHECK(database.scanNow());
    mesh = database.find(stableId);
    CHECK(mesh != nullptr);
    CHECK(database.importForAsset(stableId)->state == EditorAssetImportState::Ready);
    database.close();
    CHECK(database.open(fixture.project().string()));
    CHECK(database.scanNow());
    CHECK(database.importForAsset(stableId) != nullptr);
    CHECK(database.importForAsset(stableId)->record.options.sourceCoordinates.tag == "external-recipe");
}

TEST_CASE(selected_asset_inspector_updates_when_a_scan_discovers_its_import_source)
{
    EditorImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    const auto imported = Importer::importAssetFile(options);
    CHECK(imported.success);
    std::wstring meshName;
    for (const auto& resource : imported.conversion.resources) {
        if (resource.type != "Mesh") continue;
        meshName = std::filesystem::path(resource.path).filename().wstring();
        break;
    }
    CHECK_FALSE(meshName.empty());
    std::filesystem::remove(fixture.output() / "Triangle.dep");
    ayt::ui::MockRenderer backend;
    EditorSession session;
    EditorSessionDesc description;
    description.uiBackend = &backend;
    description.projectRoot = fixture.project().string();
    description.layoutPath = std::string(AY_EDITOR_TEST_SOURCE_DIR) + "/ui/editor_shell.ui.json";
    CHECK(session.initialize(description));
    session.setClientSize(1280, 720);
    CHECK(session.rescanAssetsNow());
    auto* mode = dynamic_cast<ayt::ui::ComboBox*>(session.ui().findById("cmb_assets_view"));
    auto* list = dynamic_cast<ayt::ui::TileView*>(session.ui().findById("list_assets"));
    auto* reload = dynamic_cast<ayt::ui::Button*>(session.ui().findById("btn_asset_reload"));
    CHECK(mode && list && reload);
    if (!mode || !list || !reload) return;
    mode->setSelectedIndexAndNotify(1);
    session.update(0);
    bool found = false;
    for (std::size_t index = 0; index < list->getItemCount(); ++index) {
        if (list->getItem(index).find(meshName) == std::wstring::npos) continue;
        list->setSelectedIndex(static_cast<int>(index));
        found = true;
        break;
    }
    CHECK(found);
    if (!found) { session.shutdown(); return; }
    CHECK(session.selectedAssetId() != 0);
    const auto stableId = session.selectedAssetId();
    CHECK(reload->isEnabled());
    CHECK(session.assetDatabase().importForAsset(stableId) == nullptr);
    options.force = true;
    CHECK(Importer::importAssetFile(options).success);
    CHECK(session.rescanAssetsNow());
    CHECK(session.selectedAssetId() == stableId);
    CHECK(reload->isEnabled());
    CHECK(session.assetDatabase().importForAsset(stableId) != nullptr);
    fixture.triangle(2.0f);
    session.ui().layout();
    const auto bounds = reload->getWorldBounds();
    const auto point = session.ui().logicalToPhysical(
        {(bounds.minX + bounds.maxX) * .5f, (bounds.minY + bounds.maxY) * .5f});
    session.onMouseMove(point.x, point.y);
    session.onMouseButtonDown(point.x, point.y, 0);
    session.onMouseButtonUp(point.x, point.y, 0);
    CHECK(session.assetImportQueue()->jobs().size() == 1u);
    if (session.assetImportQueue()->jobs().empty()) { session.shutdown(); return; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (session.assetImportQueue()->busy() && std::chrono::steady_clock::now() < deadline) {
        session.update(1.0f / 60);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(session.assetImportQueue()->busy());
    CHECK(session.assetImportQueue()->jobs().back().state == EditorAssetImportJobState::Succeeded);
    session.shutdown();
}

TEST_CASE(project_import_recipe_survives_cache_deletion_and_project_relocation)
{
    EditorImportFixture fixture;
    fixture.triangle();
    const auto source = fixture.project() / "Assets/Models/Triangle.gltf";
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::copy_file(fixture.source(), source);
    std::filesystem::copy_file(fixture.source().parent_path() / "Triangle.bin",
        source.parent_path() / "Triangle.bin");
    auto options = fixture.options();
    options.sourcePath = source.string();
    options.materialPolicy.tag = "durable-authoring-settings";
    options.materialPolicy.maskNames = "Grate";
    options.sourceCoordinates.tag = "persisted-coordinates";
    const auto sourceRoot = fixture.project() / "Assets";
    {
        EditorAssetImportQueue queue;
        queue.configureProject(fixture.project().string(), sourceRoot.string());
        const auto id = queue.enqueue(options);
        CHECK(fixture.finish(queue));
        CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
    }
    const auto recipes = detail::loadImportRecipes(fixture.project().string());
    CHECK(recipes.size() == 1u);
    if (recipes.empty()) return;
    CHECK(recipes[0].options.materialPolicy.tag == options.materialPolicy.tag);
    CHECK(recipes[0].options.sourceCoordinates.tag == options.sourceCoordinates.tag);
    CHECK(std::filesystem::is_regular_file(recipes[0].path));
    // Only this fixture's explicitly derived cache is removed; source and
    // .ayeditor/imports author data remain in the temporary project.
    std::filesystem::remove_all(fixture.output());
    const auto moved = fixture.root / "RelocatedProject";
    std::filesystem::rename(fixture.project(), moved);
    const auto relocated = detail::loadImportRecipes(moved.string());
    CHECK(relocated.size() == 1u);
    if (relocated.empty()) return;
    CHECK(relocated[0].id == recipes[0].id);
    CHECK(std::filesystem::equivalent(std::filesystem::u8path(relocated[0].options.sourcePath),
        moved / "Assets/Models/Triangle.gltf"));
    EditorAssetDatabase database;
    CHECK(database.open(moved.string()));
    CHECK(database.scanNow());
    CHECK(database.imports().size() == 1u);
    CHECK(database.imports()[0].state == EditorAssetImportState::NeedsImport);
    const auto* record = database.findByLogicalPath("Assets/Models/Triangle.gltf");
    CHECK(record && database.importForAsset(record->id));
    EditorAssetImportQueue queue;
    queue.configureProject(moved.string(), (moved / "Assets").string());
    CHECK(queue.enqueueChangedDependencies(database) == 1u);
    CHECK(fixture.finish(queue));
    CHECK(queue.jobs()[0].state == EditorAssetImportJobState::Succeeded);
    CHECK(queue.jobs()[0].options.materialPolicy.maskNames == "Grate");
    CHECK(queue.jobs()[0].options.sourceCoordinates.tag == "persisted-coordinates");
    CHECK(database.scanNow());
    CHECK(database.imports()[0].state == EditorAssetImportState::Ready);
}

TEST_CASE(import_settings_form_validates_pending_fields_before_applying)
{
    ayt::ui::MockRenderer backend;
    ayt::ui::UIManager manager;
    manager.initialize(&backend);
    manager.setClientSize(1280, 720);
    ayt::resource::ImportOptions original;
    original.sourcePath = "Source/Model.gltf";
    original.outputDir = "Imported";
    original.requireCharacterAssets = false;
    original.sourceCoordinates.tag = "preserve-source-tag";
    original.materialPolicy.opaqueIndices = "2;5";
    original.sourceCoordinates.metersPerUnit = 1e-8f;
    int applications = 0;
    ayt::resource::ImportOptions applied;
    EditorImportSettingsDialog dialog(original,
        [&](const auto& options, std::string&) { ++applications; applied = options; return true; },
        [](const char*, const wchar_t* fallback) { return std::wstring(fallback); });
    manager.root()->addChildExternal(&dialog);
    dialog.fitAndCenterInViewport({1280, 720}, 12);
    dialog.openModal();
    const auto find = [](auto&& self, ayt::ui::Widget* widget, const char* id) -> ayt::ui::Widget* {
        if (widget->getId() == id) return widget;
        for (auto* child : widget->getChildren()) if (auto* result = self(self, child, id)) return result;
        return nullptr;
    };
    const auto widget = [&](const char* id) { return find(find, dialog.getBodyContent(), id); };
    auto* units = dynamic_cast<ayt::ui::TextInput*>(widget("import_settings_units_input"));
    auto* mode = dynamic_cast<ayt::ui::ComboBox*>(widget("import_settings_coordinates"));
    auto* up = dynamic_cast<ayt::ui::ComboBox*>(widget("import_settings_up"));
    auto* forward = dynamic_cast<ayt::ui::ComboBox*>(widget("import_settings_forward"));
    auto* normal = dynamic_cast<ayt::ui::ComboBox*>(widget("import_settings_normal_y"));
    auto* mask = dynamic_cast<ayt::ui::TextInput*>(widget("import_settings_mask_input"));
    auto* error = dynamic_cast<ayt::ui::TextLabel*>(widget("import_settings_error"));
    CHECK(units && mode && up && forward && normal && mask && error);
    if (!units || !mode || !up || !forward || !normal || !mask || !error) {
        dialog.closeModal(); manager.root()->removeChild(&dialog); manager.shutdown(); return;
    }
    CHECK_FLOAT_EQ(std::stof(units->getText()), original.sourceCoordinates.metersPerUnit, 1e-12f);
    units->setText(L"nan");
    dialog.acceptDialog();
    CHECK(dialog.isOpen());
    CHECK(applications == 0);
    CHECK_FALSE(error->getText().empty());
    units->setText(L"0.01");
    mode->setSelectedIndexAndNotify(1);
    up->setSelectedIndexAndNotify(2);
    forward->setSelectedIndexAndNotify(3); // Both directions belong to the Y axis.
    dialog.acceptDialog();
    CHECK(dialog.isOpen());
    CHECK(applications == 0);
    forward->setSelectedIndexAndNotify(4);
    normal->setSelectedIndexAndNotify(1);
    mask->setText(L"Grate;Fence"); // Pending text is read even without Enter/focus loss.
    dialog.acceptDialog();
    CHECK_FALSE(dialog.isOpen());
    CHECK(applications == 1);
    CHECK_FLOAT_EQ(applied.sourceCoordinates.metersPerUnit, .01f, 1e-6f);
    CHECK(applied.sourceCoordinates.tag == original.sourceCoordinates.tag);
    CHECK(applied.materialPolicy.normalMapYSign == -1);
    CHECK(applied.materialPolicy.maskNames == "Grate;Fence");
    CHECK(applied.materialPolicy.opaqueIndices == "2;5");
    manager.root()->removeChild(&dialog);
    manager.shutdown();
}

TEST_CASE(project_imports_isolate_two_sources_with_the_same_filename)
{
    EditorImportFixture fixture;
    fixture.triangle();
    std::filesystem::create_directories(fixture.project() / "Assets");
    const auto other = fixture.root / "Other/Triangle.gltf";
    std::filesystem::create_directories(other.parent_path());
    std::filesystem::copy_file(fixture.source(), other);
    std::filesystem::copy_file(fixture.source().parent_path() / "Triangle.bin", other.parent_path() / "Triangle.bin");
    EditorAssetImportQueue queue;
    queue.configureProject(fixture.project().string(), (fixture.project() / "Assets").string());
    auto options = fixture.options();
    const auto firstId = queue.enqueue(options);
    CHECK(fixture.finish(queue));
    CHECK(queue.find(firstId)->state == EditorAssetImportJobState::Succeeded);
    const auto firstNamespace = queue.find(firstId)->options.outputNamespace;
    const auto firstOutputs = queue.find(firstId)->outputPaths;
    CHECK_FALSE(firstNamespace.empty());
    options.sourcePath = other.string();
    const auto secondId = queue.enqueue(options);
    CHECK(fixture.finish(queue));
    CHECK(queue.find(secondId)->state == EditorAssetImportJobState::Succeeded);
    CHECK_FALSE(queue.find(secondId)->options.outputNamespace.empty());
    CHECK(queue.find(secondId)->options.outputNamespace != firstNamespace);
    CHECK(queue.find(secondId)->outputPaths != firstOutputs);
    CHECK(detail::loadImportRecipes(fixture.project().string()).size() == 2u);
    // The ordinary source command recovers the same namespace and settings.
    const auto reimportId = queue.enqueue(fixture.source().string(), fixture.output().string());
    CHECK(fixture.finish(queue));
    CHECK(queue.find(reimportId)->options.outputNamespace == firstNamespace);
    CHECK(queue.find(reimportId)->state == EditorAssetImportJobState::CacheHit);
}

TEST_CASE(project_recipe_identity_survives_relocation_with_existing_derived_outputs)
{
    EditorImportFixture fixture;
    fixture.triangle();
    const auto source = fixture.project() / "Assets/Models/Triangle.gltf";
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::copy_file(fixture.source(), source);
    std::filesystem::copy_file(fixture.source().parent_path() / "Triangle.bin", source.parent_path() / "Triangle.bin");
    auto options = fixture.options();
    options.sourcePath = source.string();
    std::string identity, outputNamespace;
    std::vector<std::string> outputs;
    {
        EditorAssetImportQueue queue;
        queue.configureProject(fixture.project().string(), (fixture.project() / "Assets").string());
        const auto id = queue.enqueue(options);
        CHECK(fixture.finish(queue));
        CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
        identity = queue.find(id)->options.sourceId;
        outputNamespace = queue.find(id)->options.outputNamespace;
        outputs = queue.find(id)->outputPaths;
    }
    const auto moved = fixture.root / "MovedProject";
    std::filesystem::rename(fixture.project(), moved);
    EditorAssetDatabase database;
    CHECK(database.open(moved.string()));
    CHECK(database.scanNow());
    EditorAssetImportQueue queue;
    queue.configureProject(moved.string(), (moved / "Assets").string());
    CHECK(database.imports().size() == 1u);
    // Use the persisted recipe, not an absolute source path from the old cache.
    const auto recipes = detail::loadImportRecipes(moved.string());
    CHECK(recipes.size() == 1u);
    if (recipes.empty()) return;
    const auto id = queue.enqueue(recipes[0].options);
    CHECK(fixture.finish(queue));
    CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
    CHECK(queue.find(id)->options.sourceId == identity);
    CHECK(queue.find(id)->options.outputNamespace == outputNamespace);
    CHECK(queue.find(id)->outputPaths == outputs);
    CHECK(database.scanNow());
}


TEST_CASE(import_queue_keeps_changed_settings_and_force_requests)
{
    EditorImportFixture fixture;
    fixture.triangle();
    EditorAssetImportQueue queue;
    auto options = fixture.options();
    const auto first = queue.enqueue(options);
    CHECK(queue.enqueue(options) == first);
    options.force = true;
    const auto forced = queue.enqueue(options);
    CHECK(forced != first);
    CHECK(queue.enqueue(options) == forced);
    options.sourceCoordinates.metersPerUnit = .01f;
    const auto changed = queue.enqueue(options);
    CHECK(changed != first && changed != forced);
    CHECK(fixture.finish(queue));
    CHECK(queue.find(first)->finished());
    CHECK(queue.find(forced)->finished());
    CHECK(queue.find(changed)->state == EditorAssetImportJobState::Succeeded);
}


TEST_CASE(project_recipe_file_changes_refresh_import_settings_without_listing_author_data_as_assets)
{
    EditorImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.sourceAssetRoot.clear();
    std::filesystem::create_directories(fixture.project()/"Assets");
    EditorAssetImportQueue queue;
    queue.configureProject(fixture.project().string(), (fixture.project()/"Assets").string());
    const auto id = queue.enqueue(options);
    CHECK(fixture.finish(queue));
    CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
    EditorAssetDatabase database;
    CHECK(database.open(fixture.project().string()));
    CHECK(database.scanNow());
    CHECK(database.imports().size()==1u);
    if(database.imports().size()!=1)return;
    CHECK(database.imports()[0].state==EditorAssetImportState::Ready);
    const auto count=database.records().size();
    options=queue.find(id)->options;
    options.sourceCoordinates.metersPerUnit=.25f;
    CHECK(detail::saveImportRecipe(fixture.project().string(),options));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    bool observed=false;
    while(std::chrono::steady_clock::now()<deadline){
        (void)database.pollFileChanges();
        (void)database.pollScan();
        if(database.imports().size()==1u
            && database.imports()[0].record.options.sourceCoordinates.metersPerUnit==.25f){
            observed=true;break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(observed);
    CHECK(database.records().size()==count);
    if(observed)CHECK(database.imports()[0].state==EditorAssetImportState::NeedsImport);
}

TEST_SUITE_END
