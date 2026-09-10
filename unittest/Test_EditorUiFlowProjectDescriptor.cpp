#include "AYTest.h"

#include <AYEditor/EditorProjectDescriptor.h>
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

TEST_SUITE_END
