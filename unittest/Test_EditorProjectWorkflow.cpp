#include "AYTest.h"

#include "AYEditor/EditorAssetTrash.h"
#include "AYEditor/EditorAssetOperations.h"
#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorProjectAssetFactory.h"
#include "AYEditor/EditorProjectDescriptor.h"
#include "AYEditor/EditorProjectRunner.h"
#include "AYEditor/EditorProjectRuntimeValidator.h"
#include "AYEditor/EditorRecoveryStore.h"
#include "AYEditor/EditorSceneDocument.h"
#include "AYEditor/EditorSelection.h"
#include "AYEditor/EditorSelectionContext.h"
#include "AYEditor/EditorWorkspace.h"
#include "../src/AYEditorTileAtlasPicker.h"

#include <AY2DEditor/TilemapEditorModel.h>
#include <AYResource/assetsImpl/Audio.h>
#include <AYResource/assetsImpl/TilemapAsset.h>
#include <AYResource/assetsImpl/Texture.h>
#include <AYScene.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/Modal.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/UIManager.h>
#include <AYUI/Widget.h>

#include <array>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

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
    return ayt::test::testTmpDir()
        / (std::string("pw_") + suffix);
}

void writeWorkflowFile(const std::filesystem::path& path,
                       const char* contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

constexpr std::array<unsigned char, 136> kWorkflowRedPng{
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
    0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,
    0x08,0x06,0x00,0x00,0x00,0xa9,0xf1,0x9e,0x7e,0x00,0x00,0x00,
    0x4f,0x49,0x44,0x41,0x54,0x78,0x01,0x01,0x44,0x00,0xbb,0xff,
    0x00,0xff,0x00,0x00,0xff,0xff,0x00,0x00,0xff,0xff,0x00,0x00,
    0xff,0xff,0x00,0x00,0xff,0x00,0xff,0x00,0x00,0xff,0xff,0x00,
    0x00,0xff,0xff,0x00,0x00,0xff,0xff,0x00,0x00,0xff,0x00,0xff,
    0x00,0x00,0xff,0xff,0x00,0x00,0xff,0xff,0x00,0x00,0xff,0xff,
    0x00,0x00,0xff,0x00,0xff,0x00,0x00,0xff,0xff,0x00,0x00,0xff,
    0xff,0x00,0x00,0xff,0xff,0x00,0x00,0xff,0x3c,0x40,0x1f,0xe1,
    0x52,0xed,0xff,0xa2,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,
    0xae,0x42,0x60,0x82};

void writeWorkflowPng(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(kWorkflowRedPng.data()),
                 static_cast<std::streamsize>(kWorkflowRedPng.size()));
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

class TilemapTestHostServices final
    : public IEditorHostServices,
      public IEditorDocumentSavePathProvider {
public:
    explicit TilemapTestHostServices(EditorWorkspace& workspace)
        : _workspace(workspace) {}

    EditorWorkspace& workspace() noexcept override { return _workspace; }
    const std::string& projectRoot() const noexcept override {
        return _projectRoot;
    }
    void requestRepaint() override { ++repaintRequests; }
    void setStatusText(const std::wstring& text) override {
        statusText = text;
    }
    ayt::ui::UIManager* uiManager() noexcept override { return ui; }
    EditorAuthoringImage loadAuthoringImage(
        const std::string&, std::string* error = nullptr) override {
        if (error != nullptr) error->clear();
        return authoringImage;
    }
    std::string chooseDocumentSavePath(
        const IEditorDocument&, bool saveAs) override {
        ++savePathRequests;
        lastSaveAs = saveAs;
        return savePath;
    }

    int repaintRequests = 0;
    int savePathRequests = 0;
    bool lastSaveAs = false;
    std::string savePath;
    std::wstring statusText;
    ayt::ui::UIManager* ui = nullptr;
    EditorAuthoringImage authoringImage;

private:
    EditorWorkspace& _workspace;
    std::string _projectRoot;
};

ayt::ui::Widget* findWorkflowWidget(ayt::ui::Widget* root,
                                    const std::string& id)
{
    if (root == nullptr) return nullptr;
    if (root->getId() == id) return root;
    for (ayt::ui::Widget* child : root->getChildren()) {
        if (auto* found = findWorkflowWidget(child, id)) return found;
    }
    return nullptr;
}

} // namespace

TEST_SUITE(AYEditor_ProjectWorkflow)

TEST_CASE(editor_source_abi_is_explicit)
{
    CHECK(kEditorSourceAbiVersion == 21u);
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
        "  \"editor\": {\"defaultSceneView\": \"2D\"},\n"
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
    CHECK(descriptor.defaultSceneView == "2D");
    CHECK(descriptor.findWorld("main") != nullptr);
    CHECK(descriptor.findWorld("main")->ui == "ui/main.ui.json");
    CHECK(descriptor.run.executable == "out/Sample.exe");
}

TEST_CASE(project_startup_scene_resolution_opens_the_declared_world)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("startup_scene")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    writeWorkflowFile(cleanup.root / "project.ayproject.json",
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"id\": \"sample\",\n"
        "  \"paths\": {\"assets\": \"Assets\"},\n"
        "  \"startupWorld\": \"main\",\n"
        "  \"worlds\": [{\"id\": \"main\", \"scene\": \"worlds/main.ayscene\"}]\n"
        "}\n");
    writeWorkflowFile(cleanup.root / "Assets/worlds/main.ayscene",
        "{\n"
        "  \"__schemaVersion\": 2,\n"
        "  \"__coordConvention\": \"ay-coordinates-lh-yup-zfwd-ccw-uvtop-m-v1\",\n"
        "  \"entities\": [{\"id\": 1, \"name\": \"Startup Entity\", \"components\": []}]\n"
        "}\n");

    const EditorProjectStartupSceneResolution resolution =
        resolveEditorProjectStartupScene(cleanup.root.string());
    CHECK(resolution.projectDescriptorPresent);
    CHECK(resolution);
    CHECK(resolution.error.empty());
    CHECK(std::filesystem::path(resolution.assetRootPath).lexically_normal()
          == (cleanup.root / "Assets").lexically_normal());
    CHECK(std::filesystem::path(resolution.scenePath).lexically_normal()
          == (cleanup.root / "Assets/worlds/main.ayscene").lexically_normal());

    EditorSceneDocument document;
    std::string error;
    CHECK(document.open(resolution.scenePath, &error));
    CHECK(error.empty());
    CHECK(document.title() == "main");
    CHECK(document.scene().world().findEntity("Startup Entity") != nullptr);
    CHECK_FALSE(document.isDirty());
}

TEST_CASE(project_startup_scene_failure_still_marks_project_mode)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("startup_invalid")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    writeWorkflowFile(cleanup.root / "project.ayproject.json", "{}\n");

    const EditorProjectStartupSceneResolution resolution =
        resolveEditorProjectStartupScene(cleanup.root.string());
    CHECK(resolution.projectDescriptorPresent);
    CHECK_FALSE(static_cast<bool>(resolution));
    CHECK_FALSE(resolution.error.empty());
    CHECK(resolution.scenePath.empty());
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
    const auto flow = createEditorProjectAsset(
        cleanup.root.string(), EditorAssetType::UiFlow);
    const auto scene2 = createEditorProjectAsset(
        cleanup.root.string(), EditorAssetType::Scene);

    CHECK(scene);
    CHECK(ui);
    CHECK(tilemap);
    CHECK(flow);
    CHECK(scene2);
    CHECK(std::filesystem::exists(scene.absolutePath));
    CHECK(std::filesystem::exists(ui.absolutePath));
    CHECK(std::filesystem::exists(tilemap.absolutePath));
    CHECK(std::filesystem::exists(flow.absolutePath));
    CHECK(scene.logicalPath.find("Assets/worlds/") == 0u);
    CHECK(ui.logicalPath.find("Assets/ui/") == 0u);
    CHECK(tilemap.logicalPath.find("Assets/tilemaps/") == 0u);
    CHECK(flow.logicalPath.find("Assets/ui/") == 0u);
    CHECK(scene.absolutePath != scene2.absolutePath);
    CHECK(classifyEditorAssetPath(tilemap.absolutePath)
        == EditorAssetType::Tilemap);
    CHECK(classifyEditorAssetPath(flow.absolutePath)
        == EditorAssetType::UiFlow);
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

TEST_CASE(project_runner_resolves_last_successful_project_build)
{
    ProjectWorkflowCleanup cleanup{
        projectWorkflowRoot("project_runner_build_state")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
#if defined(_WIN32)
    const char* executable = "out/package/windows-development/Game.exe";
#else
    const char* executable = "out/package/windows-development/Game";
#endif
    writeWorkflowFile(cleanup.root / executable, "placeholder");
    writeWorkflowFile(cleanup.root
        / ".ayeditor/builds/last-success.json",
        (std::string("{\"format\":\"AYProjectBuildState\",\"version\":1,")
         + "\"profile\":\"windows-development\",\"executable\":\""
         + executable + "\",\"workingDirectory\":"
         + "\"out/package/windows-development\","
         + "\"arguments\":[\"-asset-root\",\"Content\"]}").c_str());
    std::string error;
    const EditorProjectRunConfig config = EditorProjectRunner::resolve(
        cleanup.root.string(), &error);
    CHECK(config);
    CHECK(error.empty());
    CHECK(config.arguments.size() == 2u);
    CHECK(config.arguments[1] == "Content");
    CHECK(std::filesystem::path(config.source).filename()
        == "last-success.json");
}

TEST_CASE(project_runner_reports_process_liveness)
{
    CHECK(EditorProjectRunner::processState(0)
          == EditorProjectProcessState::Unavailable);
#if defined(_WIN32)
    CHECK(EditorProjectRunner::processState(GetCurrentProcessId())
          == EditorProjectProcessState::Running);
#endif
}

// H-17 (ayeditor audit 2026-09-14): run.json / project descriptor must
// not be allowed to launch executables outside the project root -- a
// hostile or accidentally-committed config pointing at cmd.exe (or any
// other absolute path outside the project) used to be silently executed.
// After the fix the resolver must reject such configs with an error and
// the resulting config must evaluate to falsy.
TEST_CASE(project_runner_rejects_executable_outside_project_root)
{
    ProjectWorkflowCleanup cleanup{
        projectWorkflowRoot("runner_outside_root")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    // Place a hostile run.json pointing at the system shell. The
    // executable exists on disk so the previous code path happily
    // accepted it; after the fix it must be rejected because the
    // canonicalized path is not inside the project root.
#if defined(_WIN32)
    const std::string hostile = "C:/Windows/System32/cmd.exe";
#else
    const std::string hostile = "/bin/sh";
#endif
    writeWorkflowFile(cleanup.root / ".ayeditor/run.json",
        (std::string("{\"executable\":\"") + hostile
         + "\",\"workingDirectory\":\".\",\"arguments\":[]}").c_str());
    std::string error;
    const EditorProjectRunConfig config = EditorProjectRunner::resolve(
        cleanup.root.string(), &error);
    CHECK(!config);
    CHECK(!error.empty());
    CHECK(error.find("project root") != std::string::npos
          || error.find("outside") != std::string::npos);
}

TEST_CASE(project_runner_rejects_working_directory_outside_project_root)
{
    ProjectWorkflowCleanup cleanup{
        projectWorkflowRoot("runner_outside_working")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
#if defined(_WIN32)
    const char* executable = "bin/TestApp.exe";
#else
    const char* executable = "bin/TestApp";
#endif
    writeWorkflowFile(cleanup.root / executable, "placeholder");
    // Same trick as the hostile-executable test, but the executable
    // itself is project-local -- the working directory is the poisoned
    // field.
#if defined(_WIN32)
    const std::string hostileWorking = "C:/Windows/System32";
#else
    const std::string hostileWorking = "/tmp";
#endif
    writeWorkflowFile(cleanup.root / ".ayeditor/run.json",
        (std::string("{\"executable\":\"") + executable
         + "\",\"workingDirectory\":\"" + hostileWorking
         + "\",\"arguments\":[]}").c_str());
    std::string error;
    const EditorProjectRunConfig config = EditorProjectRunner::resolve(
        cleanup.root.string(), &error);
    CHECK(!config);
    CHECK(!error.empty());
    CHECK(error.find("working directory") != std::string::npos
          || error.find("project root") != std::string::npos);
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

TEST_CASE(audio_timeline_edits_waveform_clips_and_keyframes_with_undo_save)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("audio_timeline_edit")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    const auto audioPath = cleanup.root / "Assets/audio/tone.ayaudio";
    ayt::resource::Audio audio;
    audio.createSineWave(440.0f, 1.0f);
    std::vector<UInt8> bytes;
    CHECK(audio.saveToBinary(bytes));
    std::filesystem::create_directories(audioPath.parent_path());
    std::ofstream output(audioPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();

    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const EditorDescriptor* descriptor = registry.find(
        kEditorAudioTimelineExtensionId);
    CHECK(descriptor != nullptr);
    EditorOpenRequest request;
    request.resourcePath = audioPath.string();
    auto document = descriptor != nullptr
        ? descriptor->createDocument(request, error) : nullptr;
    CHECK(document != nullptr);
    auto* timeline = document != nullptr
        ? dynamic_cast<IEditorTimelineSource*>(document.get()) : nullptr;
    CHECK(timeline != nullptr);
    if (timeline == nullptr) return;
    const auto tracks = timeline->timelineTracks();
    CHECK(!tracks.empty());
    CHECK(!timeline->timelineWaveformPeaks("audio").empty());
    CHECK(timeline->timelineAddKeyframe(tracks.front().id, 0.25, 0.8));
    EditorTimelineClip clip;
    clip.trackId = tracks.front().id;
    clip.sourcePath = audioPath.string();
    clip.startSeconds = 1.0;
    clip.durationSeconds = 0.5;
    CHECK(timeline->timelineAddClip(clip));
    CHECK(document->isDirty());
    CHECK(timeline->timelineCanUndo());
    CHECK(timeline->timelineUndo());
    CHECK(timeline->timelineClips().size() == 1u);
    CHECK(timeline->timelineRedo());
    CHECK(timeline->timelineClips().size() == 2u);
    CHECK(document->save(&error));
    CHECK(error.empty());
    CHECK(!document->isDirty());
    CHECK(timeline->timelineUndo());
    CHECK(document->isDirty());
    CHECK(timeline->timelineRedo());
    CHECK(!document->isDirty());
    CHECK(std::filesystem::is_regular_file(
        audioPath.string() + ".timeline.json"));

    auto reloaded = descriptor->createDocument(request, error);
    auto* reloadedTimeline = reloaded != nullptr
        ? dynamic_cast<IEditorTimelineSource*>(reloaded.get()) : nullptr;
    CHECK(reloadedTimeline != nullptr);
    CHECK(reloadedTimeline != nullptr
        && reloadedTimeline->timelineKeyframes().size() == 1u);
    CHECK(reloadedTimeline != nullptr
        && reloadedTimeline->timelineClips().size() == 2u);
}

TEST_CASE(tilemap_author_source_save_cooks_runtime_v3_visuals)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("rich_tilemap_save")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    const auto source = cleanup.root
        / "Assets/maps/rich.aytilemap.json";
    std::filesystem::create_directories(source.parent_path());
    const auto externalAtlas = cleanup.root / "External/sheet.png";
    writeWorkflowPng(externalAtlas);

    ayt::ay2d::editor::TilemapEditorModel authored;
    CHECK(authored.newDocument(4u, 3u, 16u, 16u, 0u));
    CHECK(authored.addLayer("Decoration"));
    ayt::ay2d::editor::TileAtlasSource atlas;
    atlas.atlasId = 7u;
    atlas.name = "sheet";
    atlas.sourcePath = externalAtlas.string();
    atlas.imageWidth = 4u;
    atlas.imageHeight = 4u;
    atlas.tileWidth = 4u;
    atlas.tileHeight = 4u;
    atlas.columns = 1u;
    atlas.rows = 1u;
    ayt::ay2d::editor::AtlasTileImport imported;
    imported.tileId = 10u;
    imported.name = "Grass";
    imported.sourceWidth = 4u;
    imported.sourceHeight = 4u;
    CHECK(authored.importTileAtlas(atlas, "Tiles/Terrain", {imported}));
    std::string error;
    CHECK(authored.save(source.string(), &error));

    EditorExtensionRegistry registry;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const EditorDescriptor* descriptor = registry.find(
        kEditorTilemapExtensionId);
    CHECK(descriptor != nullptr);
    if (descriptor == nullptr) return;
    EditorOpenRequest request;
    request.resourcePath = source.string();
    auto document = descriptor->createDocument(request, error);
    CHECK(document != nullptr);
    if (document == nullptr) return;
    CHECK(document->save(&error));
    CHECK(error.empty());
    CHECK_FALSE(document->isDirty());
    CHECK(std::filesystem::is_regular_file(source));
    const auto cookedPath = cleanup.root / "Assets/tilemaps/rich.aytilemap";
    CHECK(std::filesystem::is_regular_file(cookedPath));
    std::ifstream cookedFile(cookedPath, std::ios::binary | std::ios::ate);
    CHECK(cookedFile.is_open());
    const std::streamsize cookedSize = cookedFile.tellg();
    cookedFile.seekg(0, std::ios::beg);
    std::vector<ayt::math::UInt8> cookedBytes(
        static_cast<size_t>(cookedSize));
    CHECK(cookedFile.read(reinterpret_cast<char*>(cookedBytes.data()),
                          cookedSize).good());
    ayt::resource::TilemapAsset runtime;
    CHECK(runtime.loadFromBinary(cookedBytes.data(), cookedBytes.size()));
    CHECK(runtime.getLayerCount() == 2u);
    CHECK(runtime.getAtlasCount() == 1u);
    CHECK(runtime.getVisualCount() == 1u);
    const std::string runtimeTexturePath =
        runtime.getAtlasEntries()[0].sourcePath;
    CHECK(runtimeTexturePath.starts_with("textures/"));
    CHECK(runtimeTexturePath.ends_with("_tilemap.aytex"));
    const auto cookedTexturePath = cleanup.root / "Assets"
        / std::filesystem::u8path(runtimeTexturePath);
    CHECK(std::filesystem::is_regular_file(cookedTexturePath));
    std::ifstream textureFile(
        cookedTexturePath, std::ios::binary | std::ios::ate);
    CHECK(textureFile.is_open());
    const std::streamsize textureSize = textureFile.tellg();
    textureFile.seekg(0, std::ios::beg);
    std::vector<ayt::math::UInt8> textureBytes(
        static_cast<size_t>(textureSize));
    CHECK(textureFile.read(reinterpret_cast<char*>(textureBytes.data()),
                           textureSize).good());
    ayt::resource::Texture texture;
    CHECK(texture.loadFromBinary(textureBytes.data(), textureBytes.size()));
    CHECK(texture.getFormat() == ayt::resource::TextureFormat::RGBA8);
    CHECK(texture.getMipmapCount() == 1u);

    ayt::ay2d::editor::TilemapDocument portableSource;
    CHECK(portableSource.load(source.string(), &error));
    const auto* portableAtlas = portableSource.tileAtlas(7u);
    CHECK(portableAtlas != nullptr);
    CHECK(portableAtlas != nullptr
        && portableAtlas->sourcePath.starts_with("Imported/Tilemaps/"));
    CHECK(portableAtlas != nullptr
        && std::filesystem::is_regular_file(
            cleanup.root / "Assets"
            / std::filesystem::u8path(portableAtlas->sourcePath)));
}

TEST_CASE(tilemap_built_in_view_exposes_functional_workspace_controls)
{
    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const EditorDescriptor* descriptor = registry.find(
        kEditorTilemapExtensionId);
    CHECK(descriptor != nullptr);
    if (descriptor == nullptr) return;

    EditorOpenRequest request;
    request.displayPath = "Untitled Tilemap";
    auto document = descriptor->createDocument(request, error);
    CHECK(document != nullptr);
    if (document == nullptr) return;
    EditorWorkspace workspace;
    TilemapTestHostServices host(workspace);
    auto view = descriptor->createView(document, host);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    CHECK(view->inputTarget() != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_canvas") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_tile_list") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_source_sheet") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_atlas_selector") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_import_sheet") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_import_tiled") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_export_tiled") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_fit_sheet") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_stamp_selector") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_stamp_select") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_shadow_mask") == nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_tool_shadow_clear") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_tool_shadow_advanced") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_shadow_color") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_shadow_color_apply") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_layer_list") != nullptr);

    auto* eraser = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        view->rootWidget(), "tilemap_tool_eraser"));
    CHECK(eraser != nullptr);
    CHECK(eraser != nullptr && eraser->getText().empty());
    CHECK(eraser != nullptr && eraser->getIconDocument() != nullptr);
    CHECK(view->inputTarget()->onKeyDown(ayt::ui::UIKey_E));
    CHECK(eraser != nullptr
        && eraser->getAccessibilityLabel().find(L"active")
            != std::wstring::npos);
    auto* shadow = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        view->rootWidget(), "tilemap_tool_shadow"));
    CHECK(shadow != nullptr);
    CHECK(shadow != nullptr && shadow->getText().empty());
    CHECK(shadow != nullptr && shadow->getIconDocument() != nullptr);
    CHECK(view->inputTarget()->onKeyDown(ayt::ui::UIKey_H));
    CHECK(shadow != nullptr
        && shadow->getAccessibilityLabel().find(L"active")
            != std::wstring::npos);

    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_layer_add") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_layer_remove") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_layer_up") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_layer_down") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_layer_visibility") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_layer_rename") != nullptr);
    constexpr const char* iconButtonIds[]{
        "tilemap_file_save", "tilemap_file_save_as",
        "tilemap_tool_pencil", "tilemap_tool_eraser",
        "tilemap_tool_fill", "tilemap_tool_rectangle",
        "tilemap_tool_stamp", "tilemap_tool_shadow",
        "tilemap_tool_shadow_clear", "tilemap_tool_shadow_advanced",
        "tilemap_tool_grid", "tilemap_tool_collision",
        "tilemap_tool_shadow_visibility", "tilemap_tool_frame",
        "tilemap_layer_add", "tilemap_layer_remove",
        "tilemap_layer_up", "tilemap_layer_down",
        "tilemap_layer_visibility", "tilemap_layer_rename"};
    for (const char* id : iconButtonIds) {
        auto* button = dynamic_cast<ayt::ui::Button*>(
            findWorkflowWidget(view->rootWidget(), id));
        CHECK(button != nullptr);
        CHECK(button != nullptr && button->getText().empty());
        CHECK(button != nullptr && button->getIconDocument() != nullptr);
    }
    view->prepareForUiShutdown();
    view.reset();
}

TEST_CASE(tilemap_first_save_chooses_and_exposes_source_path)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("tilemap_first_save")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);

    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const EditorDescriptor* descriptor = registry.find(
        kEditorTilemapExtensionId);
    CHECK(descriptor != nullptr);
    if (descriptor == nullptr) return;

    EditorOpenRequest request;
    request.displayPath = "Untitled Tilemap";
    auto document = descriptor->createDocument(request, error);
    CHECK(document != nullptr);
    if (document == nullptr) return;

    EditorWorkspace workspace;
    TilemapTestHostServices host(workspace);
    host.savePath = (cleanup.root / "Assets" / "tilemaps"
        / "FirstSave.aytilemap.json").string();
    auto view = descriptor->createView(document, host);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    IEditorCommandTarget* commands = view->commandTarget();
    CHECK(commands != nullptr);
    CHECK(commands != nullptr && commands->canExecuteCommand("file.save"));
    CHECK(commands != nullptr && commands->executeCommand("file.save"));
    CHECK(host.savePathRequests == 1);
    CHECK_FALSE(host.lastSaveAs);
    CHECK(document->path() == host.savePath);
    CHECK(document->title() == "FirstSave.aytilemap.json");
    CHECK(std::filesystem::is_regular_file(host.savePath));

    auto* pathLabel = dynamic_cast<ayt::ui::TextLabel*>(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_document_path"));
    CHECK(pathLabel != nullptr);
    CHECK(pathLabel != nullptr
        && pathLabel->getText().find(L"FirstSave.aytilemap.json")
            != std::wstring::npos);
    CHECK(host.statusText.find(L"Tilemap saved to") != std::wstring::npos);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_file_save") != nullptr);
    CHECK(findWorkflowWidget(
        view->rootWidget(), "tilemap_file_save_as") != nullptr);

    view->prepareForUiShutdown();
    view.reset();
}

TEST_CASE(tilemap_import_modal_uses_owning_ui_and_centers_in_client)
{
    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const EditorDescriptor* descriptor = registry.find(
        kEditorTilemapExtensionId);
    CHECK(descriptor != nullptr);
    if (descriptor == nullptr) return;

    EditorOpenRequest request;
    request.displayPath = "Untitled Tilemap";
    auto document = descriptor->createDocument(request, error);
    CHECK(document != nullptr);
    if (document == nullptr) return;

    ayt::ui::UIManager primary;
    primary.initialize(nullptr);
    primary.setClientSize(1200.0f, 800.0f);
    EditorWorkspace workspace;
    TilemapTestHostServices host(workspace);
    host.ui = &primary;
    auto view = descriptor->createView(document, host);
    CHECK(view != nullptr);
    if (view == nullptr) {
        primary.shutdown();
        return;
    }

    ayt::ui::UIManager secondary;
    secondary.initialize(nullptr);
    secondary.setClientSize(500.0f, 400.0f);

    auto* importButton = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_import_sheet"));
    CHECK(importButton != nullptr);
    if (importButton != nullptr) {
        const ayt::ui::UIMouseEvent pointer(
            ayt::math::FVector2(1.0f, 1.0f), 0);
        importButton->onMouseMove(pointer);
        importButton->onMouseButtonDown(pointer);
        importButton->onMouseButtonUp(pointer);
    }

    ayt::ui::Widget* modal = findWorkflowWidget(
        primary.getOverlayRoot(), "tilemap_atlas_import_dialog");
    CHECK(modal != nullptr);
    CHECK(findWorkflowWidget(
        secondary.getOverlayRoot(), "tilemap_atlas_import_dialog") == nullptr);
    if (modal != nullptr) {
        auto activeScope = ayt::ui::UIManager::pushActive(&primary);
        primary.update(0.17f);
        auto* importModal = dynamic_cast<ayt::ui::Modal*>(modal);
        CHECK(importModal != nullptr);
        CHECK(importModal != nullptr && importModal->getContent() != nullptr);
        CHECK(importModal != nullptr && importModal->getContent() != nullptr
              && importModal->getContent()->getStyleId() == "panel_default");
        CHECK(modal->getSize().x == 930.0f);
        CHECK(modal->getSize().y == 650.0f);
        CHECK(modal->getPosition().x == 135.0f);
        CHECK(modal->getPosition().y == 75.0f);
    }

    view->prepareForUiShutdown();
    view.reset();
    secondary.shutdown();
    primary.shutdown();
}

TEST_CASE(tilemap_advanced_shadow_brush_is_spatial_and_centered)
{
    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const EditorDescriptor* descriptor = registry.find(
        kEditorTilemapExtensionId);
    CHECK(descriptor != nullptr);
    if (descriptor == nullptr) return;

    EditorOpenRequest request;
    request.displayPath = "Untitled Tilemap";
    auto document = descriptor->createDocument(request, error);
    CHECK(document != nullptr);
    if (document == nullptr) return;

    ayt::ui::UIManager primary;
    primary.initialize(nullptr);
    primary.setClientSize(1200.0f, 800.0f);
    EditorWorkspace workspace;
    TilemapTestHostServices host(workspace);
    host.ui = &primary;
    auto view = descriptor->createView(document, host);
    CHECK(view != nullptr);
    if (view == nullptr) {
        primary.shutdown();
        return;
    }

    const auto click = [](ayt::ui::Button* button) {
        if (button == nullptr) return;
        const ayt::math::FRectangle bounds = button->getWorldBounds();
        const ayt::math::FVector2 center(
            (bounds.minX + bounds.maxX) * 0.5f,
            (bounds.minY + bounds.maxY) * 0.5f);
        const ayt::ui::UIMouseEvent pointer(center, 0);
        button->onMouseMove(pointer);
        button->onMouseButtonDown(pointer);
        button->onMouseButtonUp(pointer);
    };

    auto* advanced = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        view->rootWidget(), "tilemap_tool_shadow_advanced"));
    CHECK(advanced != nullptr);
    click(advanced);

    ayt::ui::Widget* modal = findWorkflowWidget(
        primary.getOverlayRoot(), "tilemap_shadow_brush_dialog");
    CHECK(modal != nullptr);
    if (modal != nullptr) {
        auto activeScope = ayt::ui::UIManager::pushActive(&primary);
        primary.update(0.17f);
        auto* shadowModal = dynamic_cast<ayt::ui::Modal*>(modal);
        CHECK(shadowModal != nullptr);
        CHECK(shadowModal != nullptr && shadowModal->getContent() != nullptr);
        CHECK(shadowModal != nullptr && shadowModal->getContent() != nullptr
              && shadowModal->getContent()->getStyleId() == "panel_default");
        CHECK(modal->getSize().x == 360.0f);
        CHECK(modal->getSize().y == 278.0f);
        CHECK(modal->getPosition().x == 420.0f);
        CHECK(modal->getPosition().y == 261.0f);
    }

    auto* topLeft = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        modal, "tilemap_shadow_quadrant_tl"));
    auto* topRight = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        modal, "tilemap_shadow_quadrant_tr"));
    auto* bottomLeft = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        modal, "tilemap_shadow_quadrant_bl"));
    auto* bottomRight = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        modal, "tilemap_shadow_quadrant_br"));
    CHECK(topLeft != nullptr);
    CHECK(topRight != nullptr);
    CHECK(bottomLeft != nullptr);
    CHECK(bottomRight != nullptr);
    CHECK(topLeft != nullptr && topLeft->getText() == L"ON");
    CHECK(topRight != nullptr && topRight->getText() == L"ON");
    CHECK(bottomLeft != nullptr && bottomLeft->getText() == L"ON");
    CHECK(bottomRight != nullptr && bottomRight->getText() == L"ON");

    click(topLeft);
    auto* status = dynamic_cast<ayt::ui::TextLabel*>(findWorkflowWidget(
        modal, "tilemap_shadow_brush_status"));
    CHECK(topLeft != nullptr && topLeft->getText() == L"OFF");
    CHECK(status != nullptr && status->getText() == L"Brush: custom quadrants");

    auto* apply = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        modal, "tilemap_shadow_brush_apply"));
    CHECK(apply != nullptr);
    click(apply);
    {
        auto activeScope = ayt::ui::UIManager::pushActive(&primary);
        primary.update(0.17f);
    }
    CHECK(findWorkflowWidget(
        primary.getOverlayRoot(), "tilemap_shadow_brush_dialog") == nullptr);
    CHECK(advanced->getAccessibilityLabel().find(L"active")
          != std::wstring::npos);

    CHECK(view->inputTarget()->onKeyDown(ayt::ui::UIKey_H));
    auto* full = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        view->rootWidget(), "tilemap_tool_shadow"));
    CHECK(full != nullptr);
    CHECK(full != nullptr && full->getAccessibilityLabel().find(L"active")
          != std::wstring::npos);

    view->prepareForUiShutdown();
    view.reset();
    primary.shutdown();
}

TEST_CASE(tilemap_stamp_selection_preserves_top_to_bottom_source_order)
{
    ProjectWorkflowCleanup cleanup{projectWorkflowRoot("stamp_orientation")};
    std::error_code ignored;
    std::filesystem::remove_all(cleanup.root, ignored);
    const auto sourcePath = cleanup.root
        / "Assets/maps/stamp.aytilemap.json";
    std::filesystem::create_directories(sourcePath.parent_path());

    ayt::ay2d::editor::TilemapEditorModel authored;
    CHECK(authored.newDocument(4u, 4u, 16u, 16u, 0u));
    ayt::ay2d::editor::TileAtlasSource atlas;
    atlas.atlasId = 1u;
    atlas.name = "orientation";
    atlas.sourcePath = "sheet.png";
    atlas.imageWidth = 32u;
    atlas.imageHeight = 32u;
    atlas.tileWidth = 16u;
    atlas.tileHeight = 16u;
    atlas.columns = 2u;
    atlas.rows = 2u;
    CHECK(authored.importTileAtlas(atlas, "Tiles/Orientation", {
        {2u, "TopLeft", 0u, 0u, 16u, 16u, 0u},
        {3u, "TopRight", 16u, 0u, 16u, 16u, 0u},
        {4u, "BottomLeft", 0u, 16u, 16u, 16u, 0u},
        {5u, "BottomRight", 16u, 16u, 16u, 16u, 0u}}));
    CHECK(authored.save(sourcePath.string()));

    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const EditorDescriptor* descriptor = registry.find(
        kEditorTilemapExtensionId);
    CHECK(descriptor != nullptr);
    if (descriptor == nullptr) return;
    EditorOpenRequest request;
    request.resourcePath = sourcePath.string();
    auto document = descriptor->createDocument(request, error);
    CHECK(document != nullptr);
    if (document == nullptr) return;

    ayt::ui::UIManager primary;
    primary.initialize(nullptr);
    primary.setClientSize(1200.0f, 800.0f);
    EditorWorkspace workspace;
    TilemapTestHostServices host(workspace);
    host.ui = &primary;
    host.authoringImage.texture.handle = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0x1234));
    host.authoringImage.texture.width = 32;
    host.authoringImage.texture.height = 32;
    host.authoringImage.width = 32u;
    host.authoringImage.height = 32u;
    host.authoringImage.bgraPixels =
        std::make_shared<const std::vector<uint8_t>>(32u * 32u * 4u, 0xffu);
    auto view = descriptor->createView(document, host);
    CHECK(view != nullptr);
    if (view == nullptr) {
        primary.shutdown();
        return;
    }
    view->rootWidget()->setSize({1000.0f, 680.0f});
    view->rootWidget()->performLayout();

    auto* select = dynamic_cast<ayt::ui::Button*>(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_stamp_select"));
    auto* picker = dynamic_cast<EditorTileAtlasPicker*>(findWorkflowWidget(
        view->rootWidget(), "tilemap_workspace_source_sheet"));
    CHECK(select != nullptr);
    CHECK(picker != nullptr);
    if (select != nullptr && picker != nullptr) {
        const ayt::math::FRectangle selectBounds = select->getWorldBounds();
        const ayt::math::FVector2 selectPoint(
            (selectBounds.minX + selectBounds.maxX) * 0.5f,
            (selectBounds.minY + selectBounds.maxY) * 0.5f);
        const ayt::ui::UIMouseEvent selectEvent(selectPoint, 0);
        select->onMouseMove(selectEvent);
        select->onMouseButtonDown(selectEvent);
        select->onMouseButtonUp(selectEvent);

        const ayt::math::FRectangle bounds = picker->getWorldBounds();
        const float side = std::min(bounds.width(), bounds.height()) - 12.0f;
        const ayt::math::FVector2 center(
            (bounds.minX + bounds.maxX) * 0.5f,
            (bounds.minY + bounds.maxY) * 0.5f);
        const ayt::math::FVector2 topLeft(
            center.x - side * 0.5f + 1.0f,
            center.y - side * 0.5f + 1.0f);
        const ayt::math::FVector2 bottomRight(
            center.x + side * 0.5f - 1.0f,
            center.y + side * 0.5f - 1.0f);
        CHECK(picker->onMouseButtonDown(
            ayt::ui::UIMouseEvent(topLeft, 0)));
        CHECK(picker->onMouseMove(
            ayt::ui::UIMouseEvent(bottomRight, 0)));
        CHECK(picker->onMouseButtonUp(
            ayt::ui::UIMouseEvent(bottomRight, 0)));
    }

    CHECK(document->save(&error));
    CHECK(error.empty());
    ayt::ay2d::editor::TilemapDocument reloaded;
    CHECK(reloaded.load(sourcePath.string(), &error));
    const auto* stamp = reloaded.tileStamp(1u);
    CHECK(stamp != nullptr);
    if (stamp != nullptr) {
        CHECK(stamp->cells.size() == 4u);
        const auto findCell = [stamp](uint32_t tileId) {
            return std::find_if(stamp->cells.begin(), stamp->cells.end(),
                [tileId](const auto& cell) { return cell.tileId == tileId; });
        };
        const auto topLeft = findCell(2u);
        const auto topRight = findCell(3u);
        const auto bottomLeft = findCell(4u);
        const auto bottomRight = findCell(5u);
        CHECK(topLeft != stamp->cells.end());
        CHECK(topRight != stamp->cells.end());
        CHECK(bottomLeft != stamp->cells.end());
        CHECK(bottomRight != stamp->cells.end());
        CHECK(topLeft != stamp->cells.end() && topLeft->offsetCol == 0
              && topLeft->offsetRow == 0);
        CHECK(topRight != stamp->cells.end() && topRight->offsetCol == 1
              && topRight->offsetRow == 0);
        CHECK(bottomLeft != stamp->cells.end() && bottomLeft->offsetCol == 0
              && bottomLeft->offsetRow == -1);
        CHECK(bottomRight != stamp->cells.end() && bottomRight->offsetCol == 1
              && bottomRight->offsetRow == -1);
    }

    view->prepareForUiShutdown();
    view.reset();
    primary.shutdown();
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
