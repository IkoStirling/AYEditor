#include "AYTest.h"

#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/EditorAssetTilePresenter.h"
#include "AYEditor/EditorProjectAssetFactory.h"
#include "AYEditor/EditorProjectDescriptor.h"

#include <AYApplication/GameFlowDocument.h>
#include <AYIO/File.h>

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
        path = std::filesystem::temp_directory_path()
            / (std::string("ayeditor_gameflow_") + suffix + "_"
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
    return descriptor;
}

} // namespace

TEST_SUITE(AYEditor_GameFlowAssetIntegration)

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
    CHECK(authored.save(root.path.string(), &error));
    const EditorProjectDescriptor loaded =
        EditorProjectDescriptor::load(root.path.string(), &error);
    CHECK(loaded);
    CHECK(error.empty());
    CHECK(loaded.startupFlow == "flow/application.gameflow.json");
    CHECK(loaded.startupWorld == "main-menu");
    const EditorProjectWorldDescriptor* mainMenu =
        loaded.findWorld("main-menu");
    CHECK(mainMenu != nullptr);
    CHECK(mainMenu != nullptr
          && mainMenu->scene == "worlds/main-menu.ayscene");
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

    descriptor = makeGameFlowProjectDescriptor();
    descriptor.startupWorld = "missing";
    CHECK_FALSE(descriptor.validate(&error));
    CHECK(error.find("not present in worlds") != std::string::npos);

    descriptor = makeGameFlowProjectDescriptor();
    descriptor.worlds.push_back(descriptor.worlds.front());
    CHECK_FALSE(descriptor.validate(&error));
    CHECK(error.find("Duplicate project World id") != std::string::npos);
}

TEST_SUITE_END
