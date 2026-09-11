#include "AYTest.h"

#include <AYEditor/EditorProjectDescriptor.h>
#include <AYEditor/EditorProjectRuntimeValidator.h>
#include <AYEditor/EditorProjectUiFlow.h>
#include <AYUI/UIFlow.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace editor_ui_flow_project_test {

namespace fs = std::filesystem;

struct ProjectRoot {
    explicit ProjectRoot(const char* name)
        : path(fs::temp_directory_path()
               / (std::string("ayeditor_ui_flow_") + name))
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
        fs::create_directories(path);
    }

    ~ProjectRoot()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    void writeDescriptor(const std::string& source) const
    {
        std::ofstream output(path / "project.ayproject.json",
                             std::ios::binary | std::ios::trunc);
        output << source;
    }

    void writeAsset(const fs::path& relative, const std::string& source) const
    {
        const fs::path destination = path / "Assets" / relative;
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination,
                             std::ios::binary | std::ios::trunc);
        output << source;
    }

    fs::path path;
};

} // namespace editor_ui_flow_project_test

using namespace ayt::editor;
using namespace ayt::ui;

TEST_SUITE(AYEditor_UIFlowProjectContract)

TEST_CASE(project_descriptor_reads_external_flow_entry_and_world_context)
{
    editor_ui_flow_project_test::ProjectRoot project("external");
    project.writeDescriptor(R"json({
        "schemaVersion":1,
        "id":"sample",
        "paths":{"assets":"Assets"},
        "ui":{"flow":"ui/game.uiflow.json","entry":"Boot"},
        "startupWorld":"main",
        "worlds":[{
            "id":"main",
            "scene":"worlds/main.ayscene",
            "uiContext":"Gameplay"
        }]
    })json");

    std::string error;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(project.path.string(), &error);
    CHECK(static_cast<bool>(descriptor));
    CHECK(error.empty());
    CHECK(descriptor.ui.flow == "ui/game.uiflow.json");
    CHECK(descriptor.ui.entry == "Boot");
    CHECK(descriptor.findWorld("main") != nullptr);
    CHECK(descriptor.findWorld("main")->uiContext == "Gameplay");

    EditorProjectUiFlowResolution resolution;
    CHECK(resolveEditorProjectUiFlow(descriptor, resolution, &error));
    CHECK(error.empty());
    CHECK(resolution.source == EditorProjectUiFlowSource::Asset);
    CHECK(resolution.flowAsset == "ui/game.uiflow.json");
    CHECK(resolution.entry == "Boot");
    CHECK(resolution.contextForWorld("main") == "Gameplay");
}

TEST_CASE(legacy_world_ui_migrates_to_valid_world_scoped_flow)
{
    editor_ui_flow_project_test::ProjectRoot project("legacy");
    project.writeDescriptor(R"json({
        "schemaVersion":1,
        "id":"legacy_game",
        "paths":{"assets":"Assets"},
        "startupWorld":"main_menu",
        "worlds":[
            {"id":"main_menu","scene":"worlds/menu.ayscene","ui":"ui/menu.ui.json"},
            {"id":"gameplay","scene":"worlds/game.ayscene","ui":"ui/hud.ui.json"}
        ]
    })json");

    std::string error;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(project.path.string(), &error);
    CHECK(static_cast<bool>(descriptor));
    CHECK(error.empty());

    EditorProjectUiFlowResolution resolution;
    CHECK(resolveEditorProjectUiFlow(descriptor, resolution, &error));
    CHECK(error.empty());
    CHECK(resolution.source
          == EditorProjectUiFlowSource::LegacyWorldLayouts);
    CHECK(resolution.compatibilityFlow.screens.size() == 2u);
    CHECK(resolution.compatibilityFlow.contexts.size() == 2u);
    CHECK(resolution.compatibilityFlow.findScreen(
              "legacy.world.gameplay.screen") != nullptr);
    CHECK(resolution.compatibilityFlow.findScreen(
              "legacy.world.gameplay.screen")->scope == UIFlowScope::World);
    CHECK(resolution.contextForWorld("main_menu")
          == "legacy.world.main_menu.context");

    std::vector<UIFlowDiagnostic> diagnostics;
    CHECK(validateUIFlow(resolution.compatibilityFlow, &diagnostics));
    CHECK(diagnostics.empty());
    std::string encoded;
    CHECK(UIFlowSerializer::serialize(
        resolution.compatibilityFlow, encoded, &diagnostics, false));
    CHECK(encoded.find("ui/hud.ui.json") != std::string::npos);
}

TEST_CASE(project_flow_rejects_ambiguous_legacy_world_layouts)
{
    editor_ui_flow_project_test::ProjectRoot project("mixed");
    project.writeDescriptor(R"json({
        "schemaVersion":1,
        "id":"mixed",
        "ui":{"flow":"ui/game.uiflow.json"},
        "worlds":[{
            "id":"main",
            "scene":"worlds/main.ayscene",
            "ui":"ui/legacy.ui.json"
        }]
    })json");

    std::string error;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(project.path.string(), &error);
    CHECK(static_cast<bool>(descriptor));
    CHECK(error.empty());

    EditorProjectUiFlowResolution resolution;
    CHECK_FALSE(resolveEditorProjectUiFlow(descriptor, resolution, &error));
    CHECK(error.find("cannot be mixed") != std::string::npos);
    CHECK_FALSE(static_cast<bool>(resolution));
}

TEST_CASE(world_context_without_project_flow_is_rejected_by_resolution)
{
    editor_ui_flow_project_test::ProjectRoot project("orphan_context");
    project.writeDescriptor(R"json({
        "schemaVersion":1,
        "id":"orphan",
        "worlds":[{
            "id":"main",
            "scene":"worlds/main.ayscene",
            "uiContext":"Gameplay"
        }]
    })json");

    std::string error;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(project.path.string(), &error);
    CHECK(static_cast<bool>(descriptor));

    EditorProjectUiFlowResolution resolution;
    CHECK_FALSE(resolveEditorProjectUiFlow(descriptor, resolution, &error));
    CHECK(error.find("without project ui.flow") != std::string::npos);
}

TEST_CASE(project_validation_resolves_flow_layout_dependencies_for_packaging)
{
    editor_ui_flow_project_test::ProjectRoot project("asset_dependencies");
    project.writeDescriptor(R"json({
        "schemaVersion":1,
        "id":"flow_assets",
        "paths":{"assets":"Assets"},
        "ui":{"flow":"ui/game.uiflow.json","entry":"Boot"},
        "worlds":[]
    })json");
    project.writeAsset("ui/shell.ui.json", R"json({
        "animations":[{"name":"enter","tracks":[{
            "target":"shell","property":"opacity",
            "keyframes":[{"timeMs":0,"value":0},{"timeMs":100,"value":1}]
        }]}],
        "root":{"type":"Panel","id":"shell"}
    })json");
    project.writeAsset("ui/game.uiflow.json", R"json({
        "schemaVersion":1,
        "id":"game",
        "defaultEntry":"Boot",
        "layers":[{"id":"application","order":0}],
        "slots":[{"id":"application.main","layer":"application"}],
        "screens":[{
            "id":"shell","layout":"ui/shell.ui.json",
            "layer":"application","slot":"application.main",
            "scope":"application","enterAnimation":"enter"
        }],
        "contexts":[{"id":"Application","slots":[{
            "slot":"application.main","operation":"present","screen":"shell"
        }]}],
        "entries":[{"id":"Boot","contexts":["Application"]}]
    })json");

    const EditorRuntimeValidationResult headless =
        EditorProjectRuntimeValidator::validate(
            project.path.string(), EditorRuntimeValidationProfile::Headless);
    const EditorRuntimeValidationResult client =
        EditorProjectRuntimeValidator::validate(
            project.path.string(), EditorRuntimeValidationProfile::FullClient);
    CHECK(headless);
    CHECK(client);
    CHECK(client.uiFlows == 1u);
    CHECK(client.uiFlowDependencies.size() == 1u);
    if (client.uiFlowDependencies.empty()) return;
    CHECK(client.uiFlowDependencies.front().flowAsset
          == "ui/game.uiflow.json");
    CHECK(client.uiFlowDependencies.front().layoutAsset
          == "ui/shell.ui.json");
    CHECK(client.uiFlowDependencies.front().screens.size() == 1u);
    CHECK(client.uiFlowDependencies.front().screens.front() == "shell");

    project.writeDescriptor(R"json({
        "schemaVersion":1,
        "id":"flow_assets",
        "paths":{"assets":"Assets"},
        "ui":{"flow":"ui/game.uiflow.json","entry":"Missing"},
        "worlds":[]
    })json");
    const EditorRuntimeValidationResult invalidEntry =
        EditorProjectRuntimeValidator::validate(
            project.path.string(), EditorRuntimeValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(invalidEntry));
    CHECK(invalidEntry.issues.size() == 1u);
    CHECK(invalidEntry.issues.front().path.find("$.ui.entry")
          != std::string::npos);

    project.writeDescriptor("{ invalid json");
    const EditorRuntimeValidationResult malformedDescriptor =
        EditorProjectRuntimeValidator::validate(
            project.path.string(), EditorRuntimeValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(malformedDescriptor));
    CHECK(malformedDescriptor.issues.size() == 1u);
    CHECK(malformedDescriptor.issues.front().path.find(
              "project.ayproject.json") != std::string::npos);
}

TEST_SUITE_END
