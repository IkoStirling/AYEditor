#include "AYTest.h"

#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/EditorAssetTilePresenter.h"
#include "AYEditor/EditorGameFlowExtension.h"
#include "AYEditor/EditorProjectAssetFactory.h"
#include "AYEditor/EditorProjectDescriptor.h"
#include "AYEditor/EditorProjectRuntimeValidator.h"
#include "AYEditor/EditorWorkspace.h"

#include <AYApplication/GameFlowDocument.h>
#include <AYIO/File.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ListView.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ayt::editor;

namespace {

struct GameFlowAssetTestRoot {
    std::filesystem::path path;

    explicit GameFlowAssetTestRoot(const char* suffix)
    {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path = ayt::test::testTmpDir()
            / (std::string("gfa_") + suffix + "_"
               + std::to_string(nonce));
    }

    ~GameFlowAssetTestRoot()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void writeGameFlowAssetTestFile(
    const std::filesystem::path& path, std::string_view contents)
{
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

bool hasRuntimeValidationIssue(const EditorRuntimeValidationResult& result,
                               std::string_view text)
{
    return std::any_of(result.issues.begin(), result.issues.end(),
        [text](const EditorRuntimeValidationIssue& issue) {
            return issue.message.find(text) != std::string::npos;
        });
}

EditorProjectDescriptor makeGameFlowProjectDescriptor()
{
    EditorProjectDescriptor descriptor;
    descriptor.schemaVersion = kEditorProjectDescriptorSchemaVersion;
    descriptor.id = "sample";
    descriptor.displayName = "Sample";
    descriptor.engineProfile = "CLIENT_2D";
    descriptor.assetRoot = "Assets";
    descriptor.gameAssembly = "game/Sample.cpp";
    descriptor.gameCodeRoot = "src";
    descriptor.defaultSceneView = "2D";
    descriptor.startupWorld = "main-menu";
    descriptor.worlds = {{
        "main-menu", "worlds/main-menu.ayscene", {}, {}, {}}};
    descriptor.run.executable = "out/Sample.exe";
    descriptor.startupFlow = "flow/application.gameflow.json";
    descriptor.gameFlowContract = "flow/gameflow.contract.json";
    return descriptor;
}

class GameFlowViewTestHost final : public IEditorHostServices {
public:
    EditorWorkspace& workspace() noexcept override { return _workspace; }
    const std::string& projectRoot() const noexcept override {
        return _projectRoot;
    }
    void requestRepaint() override { ++repaintRequests; }
    void setStatusText(const std::wstring& value) override {
        statusText = value;
    }
    std::wstring localizedText(
        std::string_view key, std::wstring_view fallback) const override {
        if (key == "ui.editor.game_flow.save") return L"Localized Save";
        return std::wstring(fallback);
    }

    int repaintRequests = 0;
    std::wstring statusText;

private:
    EditorWorkspace _workspace;
    std::string _projectRoot;
};

ayt::ui::Widget* findGameFlowWidget(
    ayt::ui::Widget* root, std::string_view id)
{
    if (root == nullptr) return nullptr;
    if (root->getId() == id) return root;
    for (ayt::ui::Widget* child : root->getChildren()) {
        if (ayt::ui::Widget* found = findGameFlowWidget(child, id)) {
            return found;
        }
    }
    return nullptr;
}

ayt::ui::Button* findGameFlowButton(
    ayt::ui::Widget* root, std::wstring_view text)
{
    if (root == nullptr) return nullptr;
    if (auto* button = dynamic_cast<ayt::ui::Button*>(root);
        button != nullptr && button->getText() == text) {
        return button;
    }
    for (ayt::ui::Widget* child : root->getChildren()) {
        if (ayt::ui::Button* found = findGameFlowButton(child, text)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

TEST_SUITE(AYEditor_GameFlowAssetIntegration)

TEST_CASE(gameflow_view_uses_host_localization_and_canvas_accepts_widget_input)
{
    EditorDescriptor descriptor = makeEditorGameFlowDescriptor();
    EditorOpenRequest request;
    request.displayPath = "Untitled.gameflow.json";
    std::string error;
    std::shared_ptr<IEditorDocument> document =
        descriptor.createDocument(request, error);
    CHECK(document != nullptr);
    CHECK(error.empty());

    GameFlowViewTestHost host;
    std::unique_ptr<IEditorView> view = document != nullptr
        ? descriptor.createView(document, host) : nullptr;
    CHECK(view != nullptr);
    ayt::ui::Widget* root = view != nullptr ? view->rootWidget() : nullptr;
    CHECK(root != nullptr);
    auto* saveButton = findGameFlowButton(root, L"Localized Save");
    CHECK(saveButton != nullptr);
    auto* initialState = dynamic_cast<ayt::ui::ComboBox*>(
        findGameFlowWidget(root, "gameflow_property_first_choice"));
    CHECK(initialState != nullptr);
    CHECK(initialState != nullptr && initialState->isVisible());
    CHECK(initialState != nullptr && initialState->getItemCount() == 1u);
    CHECK(initialState != nullptr
        && initialState->getSelectedItem() == L"Boot");
    CHECK(dynamic_cast<ayt::ui::ListView*>(
        findGameFlowWidget(root, "gameflow_action_palette")) != nullptr);

    if (root != nullptr) {
        root->setSize({1000.0f, 700.0f});
        root->performLayout();
    }
    CHECK(saveButton != nullptr && saveButton->getHeight() >= 32.0f);
    auto* parameterPanel = findGameFlowWidget(
        root, "gameflow_parameter_panel");
    CHECK(parameterPanel != nullptr);
    CHECK(parameterPanel != nullptr && !parameterPanel->isVisible());
    ayt::ui::Widget* canvas = findGameFlowWidget(root, "gameflow_canvas");
    CHECK(canvas != nullptr);
    if (canvas != nullptr) {
        const ayt::math::FRectangle bounds = canvas->getWorldBounds();
        const ayt::math::FVector2 blankPoint{
            bounds.maxX - 16.0f, bounds.maxY - 16.0f};
        CHECK(canvas->onMouseButtonDown(
            ayt::ui::UIMouseEvent(blankPoint, 2)));
        CHECK(view->inputTarget() != nullptr);
        CHECK(view->inputTarget() != nullptr
              && view->inputTarget()->hasPointerCapture());
        CHECK(canvas->onMouseMove(ayt::ui::UIMouseEvent(
            {blankPoint.x - 12.0f, blankPoint.y - 8.0f}, 2)));
        CHECK(canvas->onMouseButtonUp(ayt::ui::UIMouseEvent(
            {blankPoint.x - 12.0f, blankPoint.y - 8.0f}, 2)));
        CHECK(view->inputTarget() != nullptr
              && !view->inputTarget()->hasPointerCapture());
    }
}

TEST_CASE(gameflow_asset_type_is_appended_and_classified_as_a_compound_suffix)
{
    CHECK(static_cast<unsigned>(EditorAssetType::UiFlow) == 13u);
    CHECK(static_cast<unsigned>(EditorAssetType::GameFlow) == 14u);
    CHECK(classifyEditorAssetPath("flow/Application.gameflow.json")
          == EditorAssetType::GameFlow);
    CHECK(classifyEditorAssetPath("FLOW/APPLICATION.GAMEFLOW.JSON")
          == EditorAssetType::GameFlow);
    CHECK(classifyEditorAssetPath("flow/application.json")
          == EditorAssetType::Unknown);
    CHECK(std::string(editorAssetTypeName(EditorAssetType::GameFlow))
          == "Game Flow");
}

TEST_CASE(gameflow_asset_tile_has_a_distinct_label_and_native_marker)
{
    EditorAssetRecord record;
    record.id = 7u;
    record.name = "application.gameflow.json";
    record.type = EditorAssetType::GameFlow;
    const EditorAssetTilePresentation tile =
        EditorAssetTilePresenter{}.present(record);
    CHECK(tile.typeAbbreviation == L"FLOW");
    CHECK(tile.category == EditorAssetTileCategory::Code);
    CHECK(tile.showEngineResourceMarker);
}

TEST_CASE(gameflow_asset_factory_writes_a_valid_production_document)
{
    GameFlowAssetTestRoot root("factory");
    const EditorProjectAssetCreateResult created = createEditorProjectAsset(
        root.path.string(), EditorAssetType::GameFlow);
    CHECK(created);
    CHECK(created.logicalPath.find("Assets/flow/") == 0u);
    CHECK(classifyEditorAssetPath(created.absolutePath)
          == EditorAssetType::GameFlow);

    ayt::app::GameFlowDocument document;
    std::vector<ayt::app::GameFlowDiagnostic> diagnostics;
    CHECK(ayt::app::GameFlowSerializer::deserialize(
        ayt::io::File::readAllText(created.absolutePath),
        document, &diagnostics));
    CHECK(diagnostics.empty());
    CHECK(document.initialState == "boot");
    CHECK(document.findIntent("app.start") != nullptr);
    CHECK(document.findState("main-menu") != nullptr);
}

TEST_CASE(asset_index_round_trips_the_appended_gameflow_type)
{
    GameFlowAssetTestRoot root("index");
    writeGameFlowAssetTestFile(
        root.path / "Assets/flow/application.gameflow.json", "{}\n");
    EditorAssetId id = 0;
    {
        EditorAssetDatabase database;
        std::string error;
        CHECK(database.open(root.path.string(), &error));
        CHECK(database.scanNow(&error));
        const EditorAssetRecord* record = database.findByLogicalPath(
            "Assets/flow/application.gameflow.json");
        CHECK(record != nullptr);
        if (record != nullptr) {
            CHECK(record->type == EditorAssetType::GameFlow);
            id = record->id;
        }
    }
    EditorAssetDatabase reopened;
    std::string error;
    CHECK(reopened.open(root.path.string(), &error));
    CHECK(reopened.loadedFromIndex());
    const EditorAssetRecord* record = reopened.findByLogicalPath(
        "Assets/flow/application.gameflow.json");
    CHECK(record != nullptr);
    CHECK(record != nullptr && record->type == EditorAssetType::GameFlow);
    CHECK(record != nullptr && record->id == id);
}

TEST_CASE(project_descriptor_round_trips_startup_flow_and_stable_world_ids)
{
    GameFlowAssetTestRoot root("descriptor");
    EditorProjectDescriptor authored = makeGameFlowProjectDescriptor();
    std::string error;
    CHECK(authored.validate(&error));
    CHECK(error.empty());

    std::string encoded;
    CHECK(authored.serialize(encoded, &error));
    CHECK(encoded.find("\"startupFlow\": \"flow/application.gameflow.json\"")
          != std::string::npos);
    CHECK(encoded.find("\"contract\": \"flow/gameflow.contract.json\"")
          != std::string::npos);
    CHECK(authored.save(root.path.string(), &error));
    const EditorProjectDescriptor loaded =
        EditorProjectDescriptor::load(root.path.string(), &error);
    CHECK(loaded);
    CHECK(error.empty());
    CHECK(loaded.startupFlow == "flow/application.gameflow.json");
    CHECK(loaded.gameFlowContract == "flow/gameflow.contract.json");
    CHECK(loaded.startupWorld == "main-menu");
    const EditorProjectWorldDescriptor* mainMenu =
        loaded.findWorld("main-menu");
    CHECK(mainMenu != nullptr);
    CHECK(mainMenu != nullptr
          && mainMenu->scene == "worlds/main-menu.ayscene");
}

TEST_CASE(project_descriptor_requires_an_exact_integer_schema_version)
{
    const std::array<std::string_view, 3> invalidVersions = {
        "true", "1.5", "4294967297"};
    std::size_t index = 0;
    for (const std::string_view version : invalidVersions) {
        const std::string suffix = "descriptor_schema_"
            + std::to_string(index++);
        GameFlowAssetTestRoot root(suffix.c_str());
        writeGameFlowAssetTestFile(root.path / kEditorProjectDescriptorFile,
            "{\"schemaVersion\":" + std::string(version)
                + ",\"id\":\"sample\"}");
        std::string error;
        const EditorProjectDescriptor descriptor =
            EditorProjectDescriptor::load(root.path.string(), &error);
        CHECK_FALSE(static_cast<bool>(descriptor));
        CHECK(error.find("integer schemaVersion 1") != std::string::npos);
    }
}

TEST_CASE(project_descriptor_rejects_unstable_flow_and_world_references)
{
    EditorProjectDescriptor descriptor = makeGameFlowProjectDescriptor();
    std::string error;

    descriptor.startupFlow = "flow/application.json";
    CHECK_FALSE(descriptor.validate(&error));
    CHECK(error.find(".gameflow.json") != std::string::npos);

    descriptor = makeGameFlowProjectDescriptor();
    descriptor.startupFlow = "flow/Application.GAMEFLOW.JSON";
    CHECK(descriptor.validate(&error));

    descriptor = makeGameFlowProjectDescriptor();
    descriptor.startupFlow = "../outside.gameflow.json";
    CHECK_FALSE(descriptor.validate(&error));
    CHECK(error.find("inside the asset root") != std::string::npos);

    constexpr std::string_view nonPortablePaths[] = {
        "flow\\application.gameflow.json",
        "..\\outside.gameflow.json",
        "C:\\outside.gameflow.json",
        "C:/outside.gameflow.json",
        "C:outside.gameflow.json",
    };
    for (const std::string_view path : nonPortablePaths) {
        descriptor = makeGameFlowProjectDescriptor();
        descriptor.startupFlow = path;
        CHECK_FALSE(descriptor.validate(&error));
        CHECK(error.find("inside the asset root") != std::string::npos);
    }

    descriptor = makeGameFlowProjectDescriptor();
    descriptor.gameFlowContract = "../outside.contract.json";
    CHECK_FALSE(descriptor.validate(&error));
    CHECK(error.find("gameFlow.contract") != std::string::npos);

    descriptor = makeGameFlowProjectDescriptor();
    descriptor.startupWorld = "missing";
    CHECK_FALSE(descriptor.validate(&error));
    CHECK(error.find("not present in worlds") != std::string::npos);

    descriptor = makeGameFlowProjectDescriptor();
    descriptor.worlds.push_back(descriptor.worlds.front());
    CHECK_FALSE(descriptor.validate(&error));
    CHECK(error.find("Duplicate project World id") != std::string::npos);
}

TEST_CASE(editor_runtime_validation_reports_the_gameflow_asset_closure)
{
    GameFlowAssetTestRoot root("runtime_validation");
    EditorProjectDescriptor descriptor;
    descriptor.schemaVersion = kEditorProjectDescriptorSchemaVersion;
    descriptor.id = "flow-validation";
    descriptor.assetRoot = "Assets";
    descriptor.startupFlow = "flow/root.gameflow.json";
    std::string error;
    CHECK(descriptor.save(root.path.string(), &error));
    writeGameFlowAssetTestFile(root.path / "Assets/flow/root.gameflow.json",
        R"json({
          "schemaVersion":2,"id":"root","initialState":"idle",
          "entryParameters":[],"result":[],"extensions":{},
          "intents":[{"id":"app.start"}],
          "states":[{"id":"idle"},{"id":"done"}],
          "transitions":[{
            "id":"go","from":"idle","intent":"app.start","to":"done",
            "actions":[{"id":"flow.enter","arguments":{
              "subflowId":"child"
            }}]
          }]
        })json");
    writeGameFlowAssetTestFile(root.path / "Assets/flow/child.gameflow.json",
        R"json({
          "schemaVersion":2,"id":"child","initialState":"idle",
          "entryParameters":[],"result":[],"extensions":{},
          "intents":[],"states":[{"id":"idle"}],"transitions":[]
        })json");

    const EditorRuntimeValidationResult result =
        EditorProjectRuntimeValidator::validate(
            root.path.string(), EditorRuntimeValidationProfile::Headless);
    CHECK(static_cast<bool>(result));
    CHECK(result.gameFlows == 2u);
    CHECK(std::any_of(result.gameFlowDependencies.begin(),
        result.gameFlowDependencies.end(), [](const auto& dependency) {
            return dependency.kind == "gameflow"
                && dependency.source
                    == "root::go::flow.enter.subflowId"
                && dependency.target == "flow/child.gameflow.json";
        }));
}

TEST_CASE(editor_runtime_validation_reports_editor_descriptor_errors)
{
    GameFlowAssetTestRoot root("invalid_descriptor");
    writeGameFlowAssetTestFile(root.path / "Assets/placeholder.txt", "ok");
    writeGameFlowAssetTestFile(root.path / kEditorProjectDescriptorFile,
        R"json({
          "schemaVersion": 1,
          "id": "invalid-editor-settings",
          "paths": { "assets": "Assets" },
          "editor": { "defaultSceneView": "Diagonal" },
          "worlds": []
        })json");

    const EditorRuntimeValidationResult result =
        EditorProjectRuntimeValidator::validate(
            root.path.string(), EditorRuntimeValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasRuntimeValidationIssue(
        result, "editor.defaultSceneView must be Auto, 2D, or 3D"));
}

TEST_CASE(editor_runtime_validation_does_not_follow_uiflow_outside_assets)
{
    GameFlowAssetTestRoot root("uiflow_symlink_escape");
    EditorProjectDescriptor descriptor;
    descriptor.schemaVersion = kEditorProjectDescriptorSchemaVersion;
    descriptor.id = "uiflow-symlink";
    descriptor.assetRoot = "Assets";
    descriptor.ui.flow = "ui/main.uiflow.json";
    std::string error;
    CHECK(descriptor.save(root.path.string(), &error));
    writeGameFlowAssetTestFile(root.path / "outside.uiflow.json", R"json({
      "schemaVersion": 1,
      "id": "outside-ui",
      "defaultEntry": "",
      "entries": [], "contexts": [], "signals": []
    })json");
    std::error_code linkError;
    std::filesystem::create_directories(root.path / "Assets/ui", linkError);
    CHECK(!linkError);
    std::filesystem::create_symlink(root.path / "outside.uiflow.json",
        root.path / "Assets/ui/main.uiflow.json", linkError);
    if (linkError) return;

    const EditorRuntimeValidationResult result =
        EditorProjectRuntimeValidator::validate(
            root.path.string(), EditorRuntimeValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasRuntimeValidationIssue(
        result, "Path resolves outside its content root"));
}

TEST_CASE(editor_runtime_validation_requires_a_regular_uiflow_asset)
{
    GameFlowAssetTestRoot root("uiflow_regular_file");
    EditorProjectDescriptor descriptor;
    descriptor.schemaVersion = kEditorProjectDescriptorSchemaVersion;
    descriptor.id = "uiflow-directory";
    descriptor.assetRoot = "Assets";
    descriptor.ui.flow = "ui/main.uiflow.json";
    std::string error;
    CHECK(descriptor.save(root.path.string(), &error));
    std::error_code filesystemError;
    std::filesystem::create_directories(
        root.path / "Assets/ui/main.uiflow.json", filesystemError);
    CHECK(!filesystemError);

    const EditorRuntimeValidationResult result =
        EditorProjectRuntimeValidator::validate(
            root.path.string(), EditorRuntimeValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasRuntimeValidationIssue(result, "must be a regular file"));
}

TEST_SUITE_END
