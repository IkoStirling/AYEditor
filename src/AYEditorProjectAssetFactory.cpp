#include "AYEditor/EditorProjectAssetFactory.h"

#include "AYEditor/EditorSceneDocument.h"
#include "AYEditor/EditorTilemapDocument.h"
#include <AYEntity/ActorClassAsset.h>

#include <AYAssetFormat/AssetFormat.h>
#include <AYApplication/GameFlowDocument.h>
#include <AYIO/File.h>
#include <AYUI/UIFlow.h>

#include <filesystem>
#include <algorithm>
#include <cctype>
#ifdef AYEDITOR_HAS_PARTICLE
#include <AYParticle/EffectAssetIO.h>
#endif
#ifdef AYEDITOR_HAS_SEQUENCE
#include <AYSequence/SequenceAssetIO.h>
#endif
#ifdef AYEDITOR_HAS_STATS
#include <AYStats/RecipeAsset.h>
#endif

namespace ayt::editor {
namespace {

struct AssetTemplate {
    const char* folder;
    const char* stem;
    std::string_view suffix;
};

bool templateFor(EditorAssetType type, AssetTemplate& value)
{
    using ayt::asset_format::Id;
    using ayt::asset_format::suffix;
    switch (type) {
#ifdef AYEDITOR_HAS_STATS
    case EditorAssetType::StatsRecipe:
        value = {"stats", "NewStats", suffix(Id::StatsSource)};
        return true;
#endif
#ifdef AYEDITOR_HAS_SEQUENCE
    case EditorAssetType::Sequence:
        value = {"sequences", "NewSequence", suffix(Id::SceneSequence)};
        return true;
#endif
#ifdef AYEDITOR_HAS_PARTICLE
    case EditorAssetType::ParticleEffect:
        value={"effects","NewParticleEffect",suffix(Id::ParticleEffect)}; return true;
#endif
    case EditorAssetType::Scene:
        value = {"worlds", "NewScene", suffix(Id::Scene)};
        return true;
    case EditorAssetType::ActorClass:
        value = {"actors", "NewActor", suffix(Id::ActorClass)};
        return true;
    case EditorAssetType::UiLayout:
        value = {"ui", "NewLayout", suffix(Id::UiLayout)};
        return true;
    case EditorAssetType::Tilemap:
        value = {"tilemaps", "NewTilemap", suffix(Id::TilemapSource)};
        return true;
    case EditorAssetType::UiFlow:
        value = {"ui", "NewFlow", suffix(Id::UiFlow)};
        return true;
    case EditorAssetType::GameFlow:
        value = {"flow", "NewGameFlow", suffix(Id::GameFlow)};
        return true;
    default:
        return false;
    }
}

std::filesystem::path uniquePath(const std::filesystem::path& folder,
                                 const AssetTemplate& value)
{
    std::filesystem::path candidate = folder
        / (std::string(value.stem) + std::string(value.suffix));
    for (unsigned serial = 2u; std::filesystem::exists(candidate); ++serial) {
        candidate = folder / (std::string(value.stem) + " "
            + std::to_string(serial) + std::string(value.suffix));
    }
    return candidate;
}

} // namespace

EditorProjectAssetCreateResult createEditorProjectAsset(
    const std::string& projectRoot, EditorAssetType type,
    const std::string& parentActorClassPath)
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
    if (!parentActorClassPath.empty()) {
        ayt::entity::ActorClassAsset parent;
        if (type != EditorAssetType::ActorClass
            || !ayt::entity::resolveActorClassAsset(
                (root / "Assets").string(), parentActorClassPath,
                parent, &result.error)) {
            if (result.error.empty()) result.error = "Invalid parent Actor class.";
            return result;
        }
    }
    const std::filesystem::path folder = root / "Assets"
        / assetTemplate.folder;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        result.error = "Could not create the project asset folder: "
            + ec.message();
        return result;
    }
    std::filesystem::path destination = uniquePath(folder, assetTemplate);
#ifdef AYEDITOR_HAS_STATS
    if (type == EditorAssetType::StatsRecipe) {
        // A Stats source has companion documents. Reserve a stem for the whole
        // set so creating another asset cannot overwrite an orphaned companion.
        for (unsigned serial = 1u;; ++serial) {
            const std::string stem = std::string(assetTemplate.stem)
                + (serial == 1u ? "" : " " + std::to_string(serial));
            const auto manifest = folder / (stem + std::string(assetTemplate.suffix));
            if (!std::filesystem::exists(manifest)
                && !std::filesystem::exists(folder / (stem + ".target.stats.json"))
                && !std::filesystem::exists(folder / (stem + ".stats-recipes.json"))
                && !std::filesystem::exists(folder / (stem + ".aystats"))) {
                destination = manifest;
                break;
            }
        }
    }
#endif
    std::string error;
    bool saved = false;
    if (type == EditorAssetType::StatsRecipe) {
#ifdef AYEDITOR_HAS_STATS
        using namespace ayt::stats;
        const std::string stem = destination.filename().string().substr(
            0, destination.filename().string().size() - assetTemplate.suffix.size());
        const auto definitionsPath = folder / (stem + ".target.stats.json");
        const auto bookPath = folder / (stem + ".stats-recipes.json");
        const auto cookedPath = folder / (stem + ".aystats");
        const auto schema = Schema::compile({{
            .name = "value", .shape = Shape::Pool, .current = 100, .capacity = 100}});
        const auto plan = CalculationPlan::compile({{.id = 1, .op = Op::Input}}, 1, 1);
        if (!schema || !plan) {
            error = !schema ? schema.error().message : plan.error().message;
        } else {
            const auto book = RecipeBook::compile({{
                "decrease", statId("value"), AttributeOp::Decrease, *plan,
                {{InputSource::Amount}}, true}});
            if (!book) error = book.error().message;
            else {
                const auto definitions = encodeDefinitions(**schema);
                const auto recipes = encodeRecipeBook(**book, **schema);
                if (!definitions || !recipes) {
                    error = !definitions ? definitions.error().message : recipes.error().message;
                } else {
                    const auto writtenDefinitions = writeTextAtomic(definitionsPath, *definitions);
                    if (!writtenDefinitions) error = writtenDefinitions.error().message;
                    else {
                        const auto writtenBook = writeTextAtomic(bookPath, *recipes);
                        if (!writtenBook) error = writtenBook.error().message;
                        else {
                            const auto source = saveRecipeAssetSource(destination, root / "Assets",
                                (std::filesystem::path("stats") / definitionsPath.filename()).generic_string(),
                                "", (std::filesystem::path("stats") / bookPath.filename()).generic_string());
                            if (source) saved = true;
                            else error = source.error().message;
                        }
                    }
                }
            }
        }
        if (!saved) {
            std::error_code ignored;
            std::filesystem::remove(destination, ignored);
            std::filesystem::remove(cookedPath, ignored);
            std::filesystem::remove(bookPath, ignored);
            std::filesystem::remove(definitionsPath, ignored);
        }
#endif
    } else if (type == EditorAssetType::Sequence) {
#ifdef AYEDITOR_HAS_SEQUENCE
        ayt::sequence::Sequence sequence; sequence.id = "new-sequence"; sequence.duration = 5;
        ayt::sequence::Diagnostic diagnostic;
        saved = ayt::sequence::saveSequence(destination.string(), sequence, &diagnostic);
        error = diagnostic.message;
#endif
    } else if (type == EditorAssetType::ParticleEffect) {
#ifdef AYEDITOR_HAS_PARTICLE
        saved=particle::saveEffectAsset(destination.string(),particle::combinedExplosion(),&error);
#endif
    } else if (type == EditorAssetType::ActorClass) {
        ayt::entity::ActorClassAsset actor;
        actor.parentPath = parentActorClassPath;
        actor.id = destination.stem().string();
        actor.id.erase(std::remove_if(actor.id.begin(), actor.id.end(),
            [](unsigned char c) { return !(std::isalnum(c) || c == '_'); }),
            actor.id.end());
        const auto script = destination.parent_path()
            / (destination.stem().string() + ".logia");
        actor.scriptPath = (std::filesystem::path("actors") / script.filename())
            .generic_string();
        const std::string source = "script " + actor.id
            + " {\n    on_start() {\n    }\n\n    on_update(dt: float) {\n    }\n}\n";
        if (std::filesystem::exists(script)) {
            error = "Actor companion script already exists.";
        } else if (!ayt::io::File::atomicWrite(script.string(), source.data(), source.size())) {
            error = "Could not create Actor companion script.";
        } else {
            saved = ayt::entity::saveActorClassAsset(destination.string(), actor, &error);
            if (!saved) std::filesystem::remove(script, ec);
        }
    } else if (type == EditorAssetType::Scene) {
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
