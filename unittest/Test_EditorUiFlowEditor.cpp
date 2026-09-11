#include "AYTest.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

#include <AYEditor/EditorUiFlowDocument.h>
#include <AYEditor/EditorUiFlowPreview.h>
#include <AYEditor/EditorUiFlowExtension.h>
#include <AYEditor/EditorExtensionRegistry.h>
#include <AYEditor/EditorAssetDatabase.h>
#include <AYEditor/EditorProjectAssetFactory.h>
#include <AYIO/File.h>
#include <AYUI/UIFlow.h>
#include <AYUI/ListView.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/Widget.h>

#include <filesystem>
#include <string>

namespace editor_ui_flow_editor_test {

namespace fs = std::filesystem;

struct TempFlow {
    explicit TempFlow(const char* name)
        : path(fs::temp_directory_path()
               / (std::string("ayeditor_flow_authoring_") + name
                  + ".uiflow.json"))
    {
        std::error_code ignored;
        fs::remove(path, ignored);
    }

    ~TempFlow()
    {
        std::error_code ignored;
        fs::remove(path, ignored);
    }

    fs::path path;
};

ayt::ui::UIFlowDocument previewFlow()
{
    using namespace ayt::ui;
    UIFlowDocument flow;
    flow.id = "preview";
    flow.defaultEntry = "Boot";
    flow.layers = {
        UIFlowLayerDefinition{"application", 0},
        UIFlowLayerDefinition{"hud", 100},
    };
    flow.slots = {
        UIFlowSlotDefinition{"application.main", "application"},
        UIFlowSlotDefinition{"hud.main", "hud"},
    };
    flow.screens = {
        UIFlowScreenDefinition{"menu", "ui/menu.ui.json", "application",
                               "application.main", UIFlowScope::Application},
        UIFlowScreenDefinition{"hud", "ui/hud.ui.json", "hud",
                               "hud.main", UIFlowScope::World},
    };
    flow.contexts = {
        UIFlowContextDefinition{
            "Menu", 0,
            {{"application.main", UIFlowSlotOperation::Present, "menu"}}},
        UIFlowContextDefinition{
            "Gameplay", 0,
            {{"hud.main", UIFlowSlotOperation::Present, "hud"}}},
    };
    flow.entries = {UIFlowEntryDefinition{"Boot", {"Menu"}, {}}};
    flow.signals = {UIFlowSignalDefinition{"start"}};
    flow.actions = {UIFlowActionDefinition{"load_world"}};
    flow.regions = {UIFlowRegionDefinition{
        "application", "menu",
        {UIFlowStateDefinition{"menu"},
         UIFlowStateDefinition{"game", {}, {}, {"Gameplay"}}}}};
    flow.transitions = {UIFlowTransitionDefinition{
        "start_game", "application", "menu", "game", "start"}};
    return flow;
}

bool hasMountedScreen(
    const std::vector<ayt::editor::EditorUiFlowPreviewScreen>& screens,
    const std::string& id)
{
    for (const auto& screen : screens) {
        if (screen.screenId == id) return true;
    }
    return false;
}

} // namespace editor_ui_flow_editor_test

using namespace ayt::editor;

TEST_SUITE(AYEditor_UIFlowAuthoring)

TEST_CASE(flow_editor_registers_as_a_complete_document_extension)
{
    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorUiFlowExtension(registry, {}, &error));
    CHECK(error.empty());
    const EditorDescriptor* descriptor = registry.find(kEditorUiFlowExtensionId);
    CHECK(descriptor != nullptr);
    CHECK(descriptor != nullptr && static_cast<bool>(descriptor->createDocument));
    CHECK(descriptor != nullptr && static_cast<bool>(descriptor->createView));
}

TEST_CASE(document_starts_valid_and_renames_references_atomically)
{
    EditorUiFlowDocument document;
    std::string error;
    CHECK(document.initialize({}, "Untitled UI Flow", &error));
    CHECK(error.empty());
    CHECK(document.isValid());

    CHECK(document.addObject(EditorUiFlowObjectKind::Screen, {}, &error));
    CHECK(document.flow().screens.size() == 1u);
    EditorUiFlowProperties screen = document.selectedProperties();
    screen.fifth = "fade-in";
    screen.sixth = "fade-out";
    CHECK(document.applySelectedProperties(screen, &error));
    CHECK(document.flow().screens.front().enterAnimation == "fade-in");
    CHECK(document.select({EditorUiFlowObjectKind::Context, "Application", {}}));
    EditorUiFlowProperties context = document.selectedProperties();
    context.first = "application.main=screen_1; !hud.main";
    CHECK(document.applySelectedProperties(context, &error));
    CHECK(document.flow().findContext("Application")->slots.size() == 2u);
    CHECK(document.select({EditorUiFlowObjectKind::Layer, "application", {}}));
    EditorUiFlowProperties properties = document.selectedProperties();
    properties.id = "base";
    CHECK(document.applySelectedProperties(properties, &error));
    CHECK(error.empty());
    CHECK(document.flow().findLayer("base") != nullptr);
    CHECK(document.flow().findSlot("application.main")->layer == "base");
    CHECK(document.flow().screens.front().layer == "base");
    CHECK(document.isValid());
}

TEST_CASE(state_transition_authoring_is_reference_safe_and_undoable)
{
    EditorUiFlowDocument document;
    std::string error;
    CHECK(document.initialize({}, {}, &error));
    CHECK(document.addObject(EditorUiFlowObjectKind::Region, {}, &error));
    const std::string regionId = document.selection().id;
    CHECK(document.addObject(EditorUiFlowObjectKind::State, regionId, &error));
    const std::string secondState = document.selection().id;
    CHECK(document.addObject(EditorUiFlowObjectKind::Signal, {}, &error));
    CHECK(document.addObject(EditorUiFlowObjectKind::Transition, {}, &error));

    EditorUiFlowProperties transition = document.selectedProperties();
    transition.third = secondState;
    transition.fifth = "preview.allowed";
    CHECK(document.applySelectedProperties(transition, &error));
    CHECK(document.isValid());

    CHECK(document.select({EditorUiFlowObjectKind::State, "idle", regionId}));
    EditorUiFlowProperties state = document.selectedProperties();
    state.id = "ready";
    CHECK(document.applySelectedProperties(state, &error));
    CHECK(document.flow().findRegion(regionId)->initialState == "ready");
    CHECK(document.flow().transitions.front().fromState == "ready");

    CHECK_FALSE(document.deleteSelection(&error));
    CHECK(error.find("referenced") != std::string::npos);
    CHECK(document.undo());
    CHECK(document.flow().findRegion(regionId)->initialState == "idle");
    CHECK(document.redo());
    CHECK(document.flow().findRegion(regionId)->initialState == "ready");
}

TEST_CASE(project_factory_and_asset_database_expose_ui_flow_assets)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / "ayeditor_flow_authoring_factory";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);

    CHECK(classifyEditorAssetPath("Menu.UIFLOW.JSON")
          == EditorAssetType::UiFlow);
    const EditorProjectAssetCreateResult created =
        createEditorProjectAsset(root.string(), EditorAssetType::UiFlow);
    CHECK(static_cast<bool>(created));
    CHECK(created.logicalPath.find(".uiflow.json") != std::string::npos);
    ayt::ui::UIFlowDocument parsed;
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    CHECK(ayt::ui::UIFlowSerializer::deserialize(
        ayt::io::File::readAllText(created.absolutePath), parsed, &diagnostics));
    CHECK(parsed.defaultEntry == "Boot");
    CHECK(diagnostics.empty());
    fs::remove_all(root, ignored);
}

TEST_CASE(document_save_load_roundtrip_preserves_authoring_model)
{
    editor_ui_flow_editor_test::TempFlow temp("roundtrip");
    EditorUiFlowDocument source;
    std::string error;
    CHECK(source.initialize({}, {}, &error));
    CHECK(source.addObject(EditorUiFlowObjectKind::Region, {}, &error));
    CHECK(source.addObject(EditorUiFlowObjectKind::Graph, {}, &error));
    const std::string graphId = source.selection().id;
    CHECK(source.addGraphNode(graphId, "host.first", &error));
    CHECK(source.addGraphNode(graphId, "host.second", &error));
    CHECK(source.connectGraphNodes(
        graphId, "node_1", "completed", "node_2", "execute", &error));
    CHECK(source.saveAs(temp.path.string(), &error));
    CHECK_FALSE(source.isDirty());

    EditorUiFlowDocument loaded;
    CHECK(loaded.initialize(temp.path.string(), temp.path.string(), &error));
    CHECK(error.empty());
    CHECK(loaded.isValid());
    CHECK(loaded.flow().regions.size() == 1u);
    CHECK(loaded.flow().graphs.size() == 1u);
    CHECK(loaded.flow().graphs.front().nodes.size() == 2u);
    CHECK(loaded.flow().graphs.front().links.size() == 1u);
    CHECK(loaded.flow().id == source.flow().id);
}

TEST_CASE(preview_uses_production_runtime_for_signal_state_and_mock_action)
{
    const ayt::ui::UIFlowDocument flow =
        editor_ui_flow_editor_test::previewFlow();
    EditorUiFlowPreview preview;
    std::string error;
    CHECK(preview.rebuild(flow, "Boot", &error));
    CHECK(error.empty());
    CHECK(preview.isRunning());
    CHECK(editor_ui_flow_editor_test::hasMountedScreen(
        preview.mountedScreens(), "menu"));
    CHECK(preview.activeStates().at("application") == "menu");

    CHECK(preview.emitSignal("start", &error));
    CHECK(preview.activeStates().at("application") == "game");
    CHECK(editor_ui_flow_editor_test::hasMountedScreen(
        preview.mountedScreens(), "hud"));
    CHECK(preview.invokeAction("load_world", &error));
    CHECK_FALSE(preview.trace().empty());
    CHECK(preview.trace().back().category == "Action");
    CHECK(preview.trace().back().id == "load_world");
}

TEST_CASE(preview_can_mount_real_layouts_into_an_editor_owned_viewport)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / "ayeditor_flow_visual_preview";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root / "ui", ignored);
    CHECK(ayt::io::File::writeAllText(
        (root / "ui" / "menu.ui.json").string(),
        R"json({"type":"Panel","id":"visual_menu","children":[{"type":"Button","id":"visual_start","text":"Start"}]})json"));
    CHECK(ayt::io::File::writeAllText(
        (root / "ui" / "hud.ui.json").string(),
        R"json({"type":"Panel","id":"visual_hud"})json"));

    ayt::ui::UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(960.0f, 540.0f);
    ayt::ui::CompoundWidget viewport;
    viewport.setSize({480.0f, 320.0f});
    manager.root()->addChildExternal(&viewport);

    EditorUiFlowPreview preview;
    preview.configureVisualHost(manager, viewport, root.string());
    const ayt::ui::UIFlowDocument flow =
        editor_ui_flow_editor_test::previewFlow();
    std::string error;
    CHECK(preview.rebuild(flow, "Boot", &error));
    CHECK(error.empty());
    CHECK(manager.findById("visual_menu") != nullptr);
    CHECK(manager.findById("visual_start") != nullptr);
    CHECK(manager.findById("visual_menu") != nullptr
          && manager.findById("visual_menu")->getSize().x == 480.0f);
    CHECK(manager.findById("visual_menu") != nullptr
          && manager.findById("visual_menu")->getSize().y == 320.0f);

    preview.clearVisualHost();
    CHECK(manager.findById("visual_menu") == nullptr);
    viewport.detachFromParent();
    fs::remove_all(root, ignored);
}

#ifdef AY_EDITOR_TEST_SOURCE_DIR
TEST_CASE(flow_editor_surfaces_asset_closure_diagnostics_per_revision)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / "ayeditor_flow_asset_diagnostics";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root / "ui", ignored);

    auto document = std::make_shared<EditorUiFlowDocument>();
    std::string error;
    CHECK(document->initialize({}, {}, &error));
    CHECK(document->addObject(EditorUiFlowObjectKind::Screen, {}, &error));

    ayt::ui::UIManager manager;
    manager.initialize(nullptr);
    manager.setClientSize(1440.0f, 860.0f);
    const fs::path chrome = fs::path(AY_EDITOR_TEST_SOURCE_DIR)
        / "ui" / "ui_flow_editor.ui.json";
    CHECK(manager.loadLayout(chrome.string()));

    EditorUiFlowExtensionConfig config;
    config.assetRoot = root.string();
    EditorUiFlowController controller(document, std::move(config));
    CHECK(controller.attach(manager));
    auto* diagnostics = dynamic_cast<ayt::ui::ListView*>(
        manager.findById("flow_diagnostics"));
    CHECK(diagnostics != nullptr);
    CHECK(diagnostics != nullptr && diagnostics->getItemCount() == 1u);
    CHECK(diagnostics != nullptr
          && diagnostics->getItem(0).find(L"ASSET ERROR")
              != std::wstring::npos);

    CHECK(ayt::io::File::writeAllText(
        (root / "ui" / "valid.ui.json").string(),
        R"json({"type":"Panel","id":"valid_screen"})json"));
    EditorUiFlowProperties properties = document->selectedProperties();
    properties.first = "ui/valid.ui.json";
    CHECK(document->applySelectedProperties(properties, &error));
    controller.tick(0.0f);
    CHECK(diagnostics->getItemCount() == 1u);
    CHECK(diagnostics->getItem(0) == L"No diagnostics");
    auto* title = dynamic_cast<ayt::ui::TextLabel*>(
        manager.findById("flow_diag_title"));
    CHECK(title != nullptr);
    CHECK(title != nullptr
          && title->getText().find(L"1 LAYOUT ASSET") != std::wstring::npos);

    controller.detach();
    fs::remove_all(root, ignored);
}
#endif

TEST_SUITE_END
