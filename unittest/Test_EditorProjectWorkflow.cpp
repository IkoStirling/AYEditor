#include "AYTest.h"

#include "AYEditor/EditorAssetTrash.h"
#include "AYEditor/EditorAssetOperations.h"
#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorProjectAssetFactory.h"
#include "AYEditor/EditorProjectDescriptor.h"
#include "AYEditor/EditorProjectRunner.h"
#include "AYEditor/EditorProjectRuntimeValidator.h"
#include "AYEditor/EditorRecoveryStore.h"
#include "AYEditor/EditorSelection.h"
#include "AYEditor/EditorSelectionContext.h"

#include <filesystem>
#include <fstream>

using namespace ayt::editor;

namespace {

struct ProjectWorkflowCleanup {
    std::filesystem::path root;
    ~ProjectWorkflowCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::filesystem::path projectWorkflowRoot(const char* suffix)
{
    return std::filesystem::temp_directory_path()
        / (std::string("ayeditor_project_workflow_") + suffix);
}

void writeWorkflowFile(const std::filesystem::path& path,
                       const char* contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

class RecoveryDocument final : public IEditorDocument {
public:
    explicit RecoveryDocument(std::string path) : _path(std::move(path)) {}
    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return true; }
    uint64_t revision() const noexcept override { return 2u; }
    bool save(std::string*) override { return false; }
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error) const override {
        writeWorkflowFile(path, "autosaved state\n");
        const bool ok = std::filesystem::is_regular_file(path);
        if (!ok && error != nullptr) *error = "write failed";
        return ok;
    }
private:
    std::string _type = "test.recovery";
    std::string _path;
    std::string _title = "Recovery Test";
};

} // namespace

TEST_SUITE(AYEditor_ProjectWorkflow)

TEST_CASE(editor_source_abi_is_explicit)
{
    CHECK(kEditorSourceAbiVersion == 2u);
    CHECK(std::string(kEditorVersion) == "0.2.0");
}

TEST_CASE(project_descriptor_exposes_authoring_world_and_run_contract)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("descriptor")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    writeWorkflowFile(cleanup.root / "project.ayproject.json",
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"id\": \"sample\",\n"
        "  \"displayName\": \"Sample\",\n"
        "  \"engineProfile\": \"CLIENT_2D\",\n"
        "  \"paths\": {\"assets\": \"Assets\", \"gameAssembly\": \"game/Sample.cpp\", \"gameCode\": \"src\"},\n"
        "  \"startupWorld\": \"main\",\n"
        "  \"worlds\": [{\"id\": \"main\", \"scene\": \"worlds/main.ayscene\", \"ui\": \"ui/main.ui.json\", \"tilemaps\": [\"tilemaps/main.aytilemap.json\"]}],\n"
        "  \"run\": {\"executable\": \"out/Sample.exe\", \"workingDirectory\": \".\", \"arguments\": []}\n"
        "}\n");
    std::string error;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(cleanup.root.string(), &error);
    CHECK(descriptor);
    CHECK(error.empty());
    CHECK(descriptor.assetRoot == "Assets");
    CHECK(descriptor.gameAssembly == "game/Sample.cpp");
    CHECK(descriptor.gameCodeRoot == "src");
    CHECK(descriptor.findWorld("main") != nullptr);
    CHECK(descriptor.findWorld("main")->ui == "ui/main.ui.json");
    CHECK(descriptor.run.executable == "out/Sample.exe");
}

TEST_CASE(project_asset_factory_creates_valid_assets_in_conventional_folders)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("factory")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);

    const auto scene = createEditorProjectAsset(
        cleanup.root.string(), EditorAssetType::Scene);
    const auto ui = createEditorProjectAsset(
        cleanup.root.string(), EditorAssetType::UiLayout);
    const auto tilemap = createEditorProjectAsset(
        cleanup.root.string(), EditorAssetType::Tilemap);
    const auto scene2 = createEditorProjectAsset(
        cleanup.root.string(), EditorAssetType::Scene);

    CHECK(scene);
    CHECK(ui);
    CHECK(tilemap);
    CHECK(scene2);
    CHECK(std::filesystem::exists(scene.absolutePath));
    CHECK(std::filesystem::exists(ui.absolutePath));
    CHECK(std::filesystem::exists(tilemap.absolutePath));
    CHECK(scene.logicalPath.find("Assets/worlds/") == 0u);
    CHECK(ui.logicalPath.find("Assets/ui/") == 0u);
    CHECK(tilemap.logicalPath.find("Assets/tilemaps/") == 0u);
    CHECK(scene.absolutePath != scene2.absolutePath);
    CHECK(classifyEditorAssetPath(tilemap.absolutePath)
        == EditorAssetType::Tilemap);
}

TEST_CASE(project_asset_trash_moves_and_restores_one_transaction)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("trash")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    const auto first = cleanup.root / "Assets/one.logia";
    const auto second = cleanup.root / "Assets/ui/two.ui.json";
    writeWorkflowFile(first, "script one\n");
    writeWorkflowFile(second, "{}\n");

    EditorAssetRecord firstRecord;
    firstRecord.absolutePath = first.string();
    firstRecord.logicalPath = "Assets/one.logia";
    EditorAssetRecord secondRecord;
    secondRecord.absolutePath = second.string();
    secondRecord.logicalPath = "Assets/ui/two.ui.json";

    EditorAssetTrash trash(cleanup.root.string());
    const EditorAssetTrashResult removed = trash.moveToTrash(
        {firstRecord, secondRecord});
    CHECK(removed);
    CHECK(removed.moved == 2u);
    CHECK(trash.canRestoreLast());
    CHECK_FALSE(std::filesystem::exists(first));
    CHECK_FALSE(std::filesystem::exists(second));

    const EditorAssetTrashResult restored = trash.restoreLast();
    CHECK(restored);
    CHECK(restored.moved == 2u);
    CHECK_FALSE(trash.canRestoreLast());
    CHECK(std::filesystem::exists(first));
    CHECK(std::filesystem::exists(second));
}

TEST_CASE(project_asset_trash_transaction_survives_editor_restart)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("trash_restart")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    const auto asset = cleanup.root / "Assets/restart.logia";
    writeWorkflowFile(asset, "restart\n");
    EditorAssetRecord record;
    record.absolutePath = asset.string();
    record.logicalPath = "Assets/restart.logia";
    {
        EditorAssetTrash trash(cleanup.root.string());
        CHECK(trash.moveToTrash({record}));
    }
    EditorAssetTrash reopened(cleanup.root.string());
    CHECK(reopened.canRestoreLast());
    CHECK(reopened.restoreLast());
    CHECK(std::filesystem::is_regular_file(asset));
}

TEST_CASE(project_asset_move_and_rename_repair_portable_references)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("asset_ops")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    writeWorkflowFile(cleanup.root / "Assets/meshes/crate.aymesh", "mesh");
    const auto scene = cleanup.root / "Assets/worlds/demo.ayscene";
    writeWorkflowFile(scene, "{\"mesh\":\"meshes/crate.aymesh\"}\n");
    EditorAssetDatabase database;
    std::string error;
    CHECK(database.open(cleanup.root.string(), &error));
    CHECK(database.scanNow(&error));
    const EditorAssetRecord* mesh = database.findByLogicalPath(
        "Assets/meshes/crate.aymesh");
    CHECK(mesh != nullptr);
    if (mesh == nullptr) return;
    EditorAssetOperations operations(cleanup.root.string());
    const auto renamed = operations.rename(database, *mesh, "box.aymesh");
    CHECK(renamed);
    CHECK(renamed.updatedReferences == 1u);
    std::ifstream input(scene, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    CHECK(text.find("meshes/box.aymesh") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        cleanup.root / "Assets/meshes/crate.aymesh"));
    CHECK(std::filesystem::exists(
        cleanup.root / "Assets/meshes/box.aymesh"));
}

TEST_CASE(project_asset_move_repairs_references_and_copy_keeps_source)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("asset_move_copy")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    const auto sourcePath = cleanup.root / "Assets/meshes/crate.aymesh";
    writeWorkflowFile(sourcePath, "mesh");
    const auto scene = cleanup.root / "Assets/worlds/demo.ayscene";
    writeWorkflowFile(scene, "{\"mesh\":\"meshes/crate.aymesh\"}\n");

    EditorAssetDatabase database;
    std::string error;
    CHECK(database.open(cleanup.root.string(), &error));
    CHECK(database.scanNow(&error));
    const EditorAssetRecord* source = database.findByLogicalPath(
        "Assets/meshes/crate.aymesh");
    CHECK(source != nullptr);
    if (source == nullptr) return;

    const EditorAssetRecord record = *source;
    EditorAssetOperations operations(cleanup.root.string());
    const auto copied = operations.copy(database, {record}, "Assets/backups");
    CHECK(copied);
    CHECK(copied.affectedAssets == 1u);
    CHECK(copied.updatedReferences == 0u);
    CHECK(std::filesystem::is_regular_file(sourcePath));
    CHECK(std::filesystem::is_regular_file(
        cleanup.root / "Assets/backups/crate.aymesh"));

    const auto moved = operations.move(database, {record}, "Assets/props");
    CHECK(moved);
    CHECK(moved.affectedAssets == 1u);
    CHECK(moved.updatedReferences == 1u);
    CHECK_FALSE(std::filesystem::exists(sourcePath));
    CHECK(std::filesystem::is_regular_file(
        cleanup.root / "Assets/props/crate.aymesh"));
    std::ifstream input(scene, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    CHECK(text.find("props/crate.aymesh") != std::string::npos);
}

TEST_CASE(project_runner_resolves_project_local_manifest)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("runner")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
#if defined(_WIN32)
    const char* executable = "bin/TestApp.exe";
#else
    const char* executable = "bin/TestApp";
#endif
    writeWorkflowFile(cleanup.root / executable, "placeholder");
    writeWorkflowFile(cleanup.root / ".ayeditor/run.json",
        (std::string("{\"executable\":\"") + executable
         + "\",\"workingDirectory\":\".\",\"arguments\":[\"--scene\",\"main\"]}").c_str());
    std::string error;
    const EditorProjectRunConfig config = EditorProjectRunner::resolve(
        cleanup.root.string(), &error);
    CHECK(config);
    CHECK(error.empty());
    CHECK(config.arguments.size() == 2u);
    CHECK(std::filesystem::path(config.workingDirectory).lexically_normal()
          == cleanup.root.lexically_normal());
}

TEST_CASE(project_runner_resolves_canonical_project_descriptor)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("project_runner_descriptor")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
#if defined(_WIN32)
    const char* executable = "bin/ProjectApp.exe";
#else
    const char* executable = "bin/ProjectApp";
#endif
    writeWorkflowFile(cleanup.root / executable, "placeholder");
    writeWorkflowFile(cleanup.root / "project.ayproject.json",
        (std::string("{\"schemaVersion\":1,\"id\":\"project\","
         "\"run\":{\"executable\":\"") + executable
         + "\",\"workingDirectory\":\".\",\"arguments\":[\"--smoke\"]}}").c_str());
    std::string error;
    const EditorProjectRunConfig config = EditorProjectRunner::resolve(
        cleanup.root.string(), &error);
    CHECK(config);
    CHECK(error.empty());
    CHECK(config.arguments.size() == 1u);
    CHECK(config.arguments[0] == "--smoke");
    CHECK(std::filesystem::path(config.source).filename()
        == "project.ayproject.json");
}

TEST_CASE(project_recovery_rotates_unclean_session_and_restores_autosave)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("recovery")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    const auto asset = cleanup.root / "Assets/script.logia";
    writeWorkflowFile(asset, "old state\n");
    RecoveryDocument document(asset.string());
    {
        EditorRecoveryStore crashed(cleanup.root.string());
        std::string error;
        CHECK(crashed.beginSession(&error));
        const auto saved = crashed.autosave({&document});
        CHECK(saved.documents == 1u);
        // Deliberately omit markCleanShutdown: emulate process termination.
    }
    EditorRecoveryStore restarted(cleanup.root.string());
    std::string error;
    CHECK(restarted.beginSession(&error));
    CHECK(restarted.hasRecoverableSession());
    const auto restored = restarted.restorePrevious();
    CHECK(restored);
    CHECK(restored.documents == 1u);
    std::ifstream input(asset, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    CHECK(text == "autosaved state\n");
    CHECK(std::filesystem::exists(asset.string() + ".before-recovery"));
    restarted.markCleanShutdown();
}

TEST_CASE(project_content_validates_scene_ui_and_tilemap_in_both_profiles)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("validation")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    CHECK(createEditorProjectAsset(cleanup.root.string(),
        EditorAssetType::Scene));
    CHECK(createEditorProjectAsset(cleanup.root.string(),
        EditorAssetType::UiLayout));
    CHECK(createEditorProjectAsset(cleanup.root.string(),
        EditorAssetType::Tilemap));
    const auto headless = EditorProjectRuntimeValidator::validate(
        cleanup.root.string(), EditorRuntimeValidationProfile::Headless);
    const auto client = EditorProjectRuntimeValidator::validate(
        cleanup.root.string(), EditorRuntimeValidationProfile::FullClient);
    CHECK(headless);
    CHECK(client);
    CHECK(headless.scenes == 1u);
    CHECK(headless.uiLayouts == 1u);
    CHECK(headless.tilemaps == 2u);
    CHECK(client.checked() == headless.checked());
}

TEST_CASE(headless_content_validation_rejects_invalid_ui_structure)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("validation_bad_ui")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    writeWorkflowFile(cleanup.root / "Assets/ui/broken.ui.json",
        "{\"id\":\"missing_type\",\"children\":[]}\n");
    const auto result = EditorProjectRuntimeValidator::validate(
        cleanup.root.string(), EditorRuntimeValidationProfile::Headless);
    CHECK(!static_cast<bool>(result));
    CHECK(result.uiLayouts == 1u);
    CHECK(result.issues.size() == 1u);
    CHECK(result.issues[0].message.find("type") != std::string::npos);
}

TEST_CASE(built_in_registry_classifies_tilemap_audio_and_timeline_surfaces)
{
    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const EditorDescriptor* tilemap = registry.find(kEditorTilemapExtensionId);
    const EditorDescriptor* audio = registry.find(kEditorAudioToolExtensionId);
    const EditorDescriptor* timeline = registry.find(kEditorTimelineToolExtensionId);
    const EditorDescriptor* animation = registry.find(
        kEditorAnimationTimelineExtensionId);
    const EditorDescriptor* audioAsset = registry.find(
        kEditorAudioTimelineExtensionId);
    CHECK(tilemap != nullptr);
    CHECK(audio != nullptr);
    CHECK(timeline != nullptr);
    CHECK(animation != nullptr);
    CHECK(audioAsset != nullptr);
    CHECK(tilemap != nullptr
        && tilemap->surfaceKind == EditorSurfaceKind::Document);
    CHECK(audio != nullptr
        && audio->surfaceKind == EditorSurfaceKind::ToolPanel);
    CHECK(timeline != nullptr
        && timeline->surfaceKind == EditorSurfaceKind::ToolPanel);

    EditorOpenRequest request;
    request.preferredEditorId = kEditorTimelineToolExtensionId;
    CHECK(registry.resolve(request) == timeline);
}

TEST_CASE(scene_selection_bridge_keeps_workspace_selection_authoritative)
{
    EditorSelectionContext context("scene.main");
    EditorSelection selection;
    selection.bind(&context);

    CHECK(selection.select(42u));
    CHECK(selection.entityId() == 42u);
    CHECK(context.primary() != nullptr);
    CHECK(context.primary()->documentId == "scene.main");
    CHECK(context.primary()->domain == "scene.entity");
    CHECK(context.primary()->objectId == "42");

    EditorObjectRef another{"scene.main", "scene.entity", "73"};
    CHECK(context.select(another));
    CHECK(selection.entityId() == 73u);
    CHECK(selection.clear());
    CHECK(context.empty());
}

TEST_SUITE_END
