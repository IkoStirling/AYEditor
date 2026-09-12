#include "AYEditor/EditorProjectAssetFactory.h"

#include "AYEditor/EditorSceneDocument.h"
#include "AYEditor/EditorTilemapDocument.h"

#include <AYApplication/GameFlowDocument.h>
#include <AYIO/File.h>
#include <AYUI/UIFlow.h>

#include <filesystem>

namespace ayt::editor {
namespace {

struct AssetTemplate {
    const char* folder;
    const char* stem;
    const char* suffix;
};

bool templateFor(EditorAssetType type, AssetTemplate& value)
{
    switch (type) {
    case EditorAssetType::Scene:
        value = {"worlds", "NewScene", ".ayscene"};
        return true;
    case EditorAssetType::UiLayout:
        value = {"ui", "NewLayout", ".ui.json"};
        return true;
    case EditorAssetType::Tilemap:
        value = {"tilemaps", "NewTilemap", ".aytilemap.json"};
        return true;
    case EditorAssetType::UiFlow:
        value = {"ui", "NewFlow", ".uiflow.json"};
        return true;
    case EditorAssetType::GameFlow:
        value = {"flow", "NewGameFlow", ".gameflow.json"};
        return true;
    default:
        return false;
    }
}

std::filesystem::path uniquePath(const std::filesystem::path& folder,
                                 const AssetTemplate& value)
{
    std::filesystem::path candidate = folder
        / (std::string(value.stem) + value.suffix);
    for (unsigned serial = 2u; std::filesystem::exists(candidate); ++serial) {
        candidate = folder / (std::string(value.stem) + " "
            + std::to_string(serial) + value.suffix);
    }
    return candidate;
}

} // namespace

EditorProjectAssetCreateResult createEditorProjectAsset(
    const std::string& projectRoot, EditorAssetType type)
{
    EditorProjectAssetCreateResult result;
    result.type = type;
    AssetTemplate assetTemplate{};
    if (!templateFor(type, assetTemplate)) {
        result.error = "This asset type has no project template.";
        return result;
    }

    std::error_code ec;
    std::filesystem::path root = projectRoot.empty()
        ? std::filesystem::current_path(ec)
        : std::filesystem::absolute(projectRoot, ec);
    if (ec) {
        result.error = "Invalid project root: " + ec.message();
        return result;
    }
    root = root.lexically_normal();
    const std::filesystem::path folder = root / "Assets"
        / assetTemplate.folder;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        result.error = "Could not create the project asset folder: "
            + ec.message();
        return result;
    }
    const std::filesystem::path destination = uniquePath(folder, assetTemplate);
    std::string error;
    bool saved = false;
    if (type == EditorAssetType::Scene) {
        EditorSceneDocument document;
        document.newScene();
        saved = document.saveAs(destination.string(), &error);
    } else if (type == EditorAssetType::Tilemap) {
        EditorTilemapDocument document;
        saved = document.create(32u, 18u, 32u, 32u, 0u)
            && document.save(destination.string(), &error);
    } else if (type == EditorAssetType::UiFlow) {
        ayt::ui::UIFlowDocument flow;
        flow.id = "new-ui-flow";
        flow.defaultEntry = "Boot";
        flow.layers = {
            ayt::ui::UIFlowLayerDefinition{"application", 0},
            ayt::ui::UIFlowLayerDefinition{"hud", 100},
        };
        flow.slots = {
            ayt::ui::UIFlowSlotDefinition{"application.main", "application"},
            ayt::ui::UIFlowSlotDefinition{"hud.main", "hud"},
        };
        flow.contexts = {ayt::ui::UIFlowContextDefinition{"Application"}};
        flow.entries = {ayt::ui::UIFlowEntryDefinition{
            "Boot", {"Application"}, {}}};
        std::string encoded;
        std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
        saved = ayt::ui::UIFlowSerializer::serialize(
            flow, encoded, &diagnostics, true)
            && ayt::io::File::atomicWrite(
                destination.string(), encoded.data(), encoded.size());
        if (!saved) error = diagnostics.empty()
            ? "Atomic UI Flow save failed."
            : diagnostics.front().message;
    } else if (type == EditorAssetType::GameFlow) {
        ayt::app::GameFlowDocument flow;
        flow.id = "new-game-flow";
        flow.initialState = "boot";
        flow.intents = {{"app.start", {}}};
        flow.states = {{"boot"}, {"main-menu"}};
        flow.transitions = {{
            "show-main-menu", "boot", "app.start", "main-menu"}};
        std::string encoded;
        std::vector<ayt::app::GameFlowDiagnostic> diagnostics;
        saved = ayt::app::GameFlowSerializer::serialize(
            flow, encoded, &diagnostics, true)
            && ayt::io::File::atomicWrite(
                destination.string(), encoded.data(), encoded.size());
        if (!saved) error = diagnostics.empty()
            ? "Atomic GameFlow save failed."
            : diagnostics.front().message;
    } else {
        static constexpr const char kUiLayout[] =
            "{\n"
            "  \"type\": \"Panel\",\n"
            "  \"id\": \"root\",\n"
            "  \"size\": { \"w\": 1280, \"h\": 720 },\n"
            "  \"children\": []\n"
            "}\n";
        saved = ayt::io::File::atomicWrite(
            destination.string(), kUiLayout, sizeof(kUiLayout) - 1u);
        if (!saved) error = "Atomic UI layout save failed.";
    }
    if (!saved) {
        result.error = error.empty() ? "Asset creation failed." : error;
        return result;
    }
    result.success = true;
    result.absolutePath = destination.lexically_normal().string();
    result.logicalPath = (std::filesystem::path("Assets")
        / assetTemplate.folder / destination.filename()).generic_string();
    return result;
}

} // namespace ayt::editor
