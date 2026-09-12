#include "AYTest.h"

#include <AYEditor/EditorUiDesignerWorkflow.h>
#include <AYUI/UIFlow.h>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <string>

namespace editor_ui_designer_workflow_test {

namespace fs = std::filesystem;

struct TempWorkflowProject {
    TempWorkflowProject()
        : root(fs::temp_directory_path() /
            ("ayeditor_ui_designer_workflow_" +
             std::to_string(++nextProject))) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        fs::create_directories(root / "ui", ignored);
    }
    ~TempWorkflowProject() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void write(const fs::path& relative, const std::string& text) const {
        std::ofstream stream(root / relative, std::ios::binary | std::ios::trunc);
        stream << text;
    }
    std::string read(const fs::path& relative) const {
        std::ifstream stream(root / relative, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()};
    }
    fs::path root;
    static inline std::atomic<unsigned long long> nextProject{0u};
};

std::string validFlow(const std::string& signal = "existing") {
    ayt::ui::UIFlowDocument flow;
    flow.id = "main";
    flow.layers.push_back({"game", 0});
    flow.slots.push_back({"main", "game", 1u, true});
    ayt::ui::UIFlowScreenDefinition screen;
    screen.id = "menu";
    screen.layoutAsset = "ui/menu.ui.json";
    screen.layer = "game";
    screen.slot = "main";
    flow.screens.push_back(screen);
    flow.signals.push_back({signal});
    std::string result;
    ayt::ui::UIFlowSerializer::serialize(flow, result, nullptr, true);
    return result;
}

TEST_SUITE(EditorUiDesignerWorkflowTests)

TEST_CASE(index_links_screens_and_completes_layout_handlers) {
    TempWorkflowProject project;
    project.write("ui/menu.ui.json", R"({
        "type":"Panel","id":"root","children":[
          {"type":"Button","id":"play","events":{"onClick":"playGame"}},
          {"type":"Button","id":"quit","events":{"onClick":"quitGame"}}
        ]
    })");
    project.write("main.uiflow.json", validFlow("playGame"));

    ayt::editor::EditorUiDesignerWorkflow workflow(project.root.string());
    std::string error;
    CHECK(workflow.refresh(&error));
    CHECK(workflow.screenLinks().size() == 1u);
    const auto links = workflow.screensForLayout(
        (project.root / "ui/menu.ui.json").string());
    CHECK(links.size() == 1u);
    CHECK(links.size() == 1u && links[0].screenId == "menu");
    const auto handlers = workflow.handlersForLayout(
        (project.root / "ui/menu.ui.json").string());
    CHECK(handlers.size() == 2u);
    const auto completions = workflow.handlerCompletions(
        (project.root / "main.uiflow.json").string(), "menu");
    CHECK(completions.size() == 2u);

    std::size_t applied = 0u;
    CHECK(workflow.applyHandlerCompletions(
        (project.root / "main.uiflow.json").string(), "menu", {},
        &applied, &error));
    CHECK(applied == 2u);
    ayt::ui::UIFlowDocument reloaded;
    CHECK(ayt::ui::UIFlowSerializer::deserialize(
        project.read("main.uiflow.json"), reloaded));
    CHECK(reloaded.screens[0].events.size() == 2u);
    CHECK(reloaded.findSignal("menu.quitGame") != nullptr);
}

TEST_CASE(handler_rename_repairs_layout_and_all_referencing_flows_atomically) {
    TempWorkflowProject project;
    project.write("ui/menu.ui.json", R"({
      "format":"AYUILayout","version":2,
      "animations":[{"name":"intro","tracks":[{
        "target":"play","property":"opacity","keyframes":[]
      }]}],
      "root":{"type":"Panel","id":"root","children":[
        {"type":"Button","id":"play","events":{"onClick":"playGame"}}
      ]}
    })");
    ayt::ui::UIFlowDocument flow;
    CHECK(ayt::ui::UIFlowSerializer::deserialize(validFlow("startGame"), flow));
    flow.screens[0].events.push_back({"playGame", "startGame"});
    std::string encoded;
    CHECK(ayt::ui::UIFlowSerializer::serialize(flow, encoded));
    project.write("main.uiflow.json", encoded);

    ayt::editor::EditorUiDesignerWorkflow workflow(project.root.string());
    std::string error;
    CHECK(workflow.refresh(&error));
    const auto handlerPlan = workflow.planRename({
        ayt::editor::EditorUiRenameKind::WidgetHandler,
        "playGame", "beginGame", (project.root / "ui/menu.ui.json").string()});
    CHECK(handlerPlan.safe);
    CHECK(handlerPlan.edits.size() == 2u);
    CHECK(workflow.applyRename(handlerPlan, &error));

    const std::string layout = project.read("ui/menu.ui.json");
    const std::string updatedFlow = project.read("main.uiflow.json");
    CHECK(layout.find("beginGame") != std::string::npos);
    CHECK(layout.find("playGame") == std::string::npos);
    CHECK(updatedFlow.find("beginGame") != std::string::npos);
}

TEST_CASE(widget_and_signal_rename_repair_typed_references) {
    TempWorkflowProject project;
    project.write("ui/menu.ui.json", R"({
      "format":"AYUILayout","version":2,
      "animations":[{"name":"intro","tracks":[{
        "target":"play","property":"opacity","keyframes":[]
      }]}],
      "root":{"type":"Panel","id":"root","children":[
        {"type":"Button","id":"play"}
      ]}
    })");
    ayt::ui::UIFlowDocument flow;
    CHECK(ayt::ui::UIFlowSerializer::deserialize(validFlow("go"), flow));
    flow.screens[0].events.push_back({"activate", "go"});
    std::string encoded;
    CHECK(ayt::ui::UIFlowSerializer::serialize(flow, encoded));
    project.write("main.uiflow.json", encoded);

    ayt::editor::EditorUiDesignerWorkflow workflow(project.root.string());
    std::string error;
    CHECK(workflow.refresh(&error));
    auto widgetPlan = workflow.planRename({
        ayt::editor::EditorUiRenameKind::WidgetId,
        "play", "play_primary", (project.root / "ui/menu.ui.json").string()});
    CHECK(widgetPlan.safe);
    CHECK(workflow.applyRename(widgetPlan, &error));
    CHECK(project.read("ui/menu.ui.json").find("play_primary")
          != std::string::npos);

    auto signalPlan = workflow.planRename({
        ayt::editor::EditorUiRenameKind::FlowSignal,
        "go", "menu.go", (project.root / "main.uiflow.json").string()});
    CHECK(signalPlan.safe);
    CHECK(workflow.applyRename(signalPlan, &error));
    ayt::ui::UIFlowDocument updated;
    CHECK(ayt::ui::UIFlowSerializer::deserialize(
        project.read("main.uiflow.json"), updated));
    CHECK(updated.findSignal("menu.go") != nullptr);
    CHECK(updated.screens[0].events[0].signal == "menu.go");
}

TEST_CASE(rename_transaction_rejects_files_changed_after_planning) {
    TempWorkflowProject project;
    project.write("ui/menu.ui.json", R"({
      "type":"Panel","id":"root","children":[
        {"type":"Button","id":"play","events":{"onClick":"playGame"}}
      ]
    })");
    project.write("main.uiflow.json", validFlow("playGame"));

    ayt::editor::EditorUiDesignerWorkflow workflow(project.root.string());
    std::string error;
    CHECK(workflow.refresh(&error));
    const auto plan = workflow.planRename({
        ayt::editor::EditorUiRenameKind::WidgetHandler,
        "playGame", "beginGame", (project.root / "ui/menu.ui.json").string()});
    CHECK(plan.safe);
    const std::string changed = project.read("ui/menu.ui.json") + "\n";
    project.write("ui/menu.ui.json", changed);
    CHECK(!workflow.applyRename(plan, &error));
    CHECK(project.read("ui/menu.ui.json") == changed);
    CHECK(project.read("main.uiflow.json").find("beginGame")
          == std::string::npos);
}

TEST_SUITE_END

} // namespace editor_ui_designer_workflow_test
