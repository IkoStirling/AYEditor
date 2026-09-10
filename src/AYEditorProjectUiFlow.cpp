#include "AYEditor/EditorProjectUiFlow.h"

#include <utility>

namespace ayt::editor {
namespace {

constexpr const char* kLegacyLayerId = "legacy.world";
constexpr const char* kLegacySlotId = "legacy.world.primary";

std::string legacyScreenId(const std::string& worldId)
{
    return "legacy.world." + worldId + ".screen";
}

std::string legacyContextId(const std::string& worldId)
{
    return "legacy.world." + worldId + ".context";
}

std::string firstDiagnosticMessage(
    const std::vector<ayt::ui::UIFlowDiagnostic>& diagnostics)
{
    if (diagnostics.empty()) return "Generated compatibility UI Flow is invalid.";
    const ayt::ui::UIFlowDiagnostic& diagnostic = diagnostics.front();
    return diagnostic.path.empty()
        ? diagnostic.message
        : diagnostic.path + ": " + diagnostic.message;
}

} // namespace

std::string_view EditorProjectUiFlowResolution::contextForWorld(
    std::string_view worldId) const noexcept
{
    for (const EditorProjectWorldUiContextBinding& binding : worldContexts) {
        if (binding.worldId == worldId) return binding.contextId;
    }
    return {};
}

bool resolveEditorProjectUiFlow(
    const EditorProjectDescriptor& descriptor,
    EditorProjectUiFlowResolution& resolution,
    std::string* error)
{
    if (error != nullptr) error->clear();
    resolution = {};

    if (!descriptor.ui.flow.empty()) {
        for (const EditorProjectWorldDescriptor& world : descriptor.worlds) {
            if (!world.ui.empty()) {
                if (error != nullptr) {
                    *error = "Project ui.flow cannot be mixed with legacy "
                        "world.ui (World '" + world.id + "').";
                }
                return false;
            }
        }
        resolution.source = EditorProjectUiFlowSource::Asset;
        resolution.flowAsset = descriptor.ui.flow;
        resolution.entry = descriptor.ui.entry;
        for (const EditorProjectWorldDescriptor& world : descriptor.worlds) {
            if (!world.uiContext.empty()) {
                resolution.worldContexts.push_back(
                    EditorProjectWorldUiContextBinding{
                        world.id, world.uiContext});
            }
        }
        return true;
    }

    for (const EditorProjectWorldDescriptor& world : descriptor.worlds) {
        if (!world.uiContext.empty()) {
            if (error != nullptr) {
                *error = "World '" + world.id
                    + "' declares uiContext without project ui.flow.";
            }
            return false;
        }
    }

    ayt::ui::UIFlowDocument generated;
    generated.id = descriptor.id.empty()
        ? "legacy.project.ui" : descriptor.id + ".legacy.ui";
    generated.layers.push_back(ayt::ui::UIFlowLayerDefinition{
        kLegacyLayerId,
        100,
        ayt::ui::UIFlowInputPolicy::ConsumeHandled,
        false,
        1u,
    });
    generated.slots.push_back(ayt::ui::UIFlowSlotDefinition{
        kLegacySlotId, kLegacyLayerId, 1u, true});

    for (const EditorProjectWorldDescriptor& world : descriptor.worlds) {
        if (world.ui.empty()) continue;
        const std::string screenId = legacyScreenId(world.id);
        const std::string contextId = legacyContextId(world.id);
        generated.screens.push_back(ayt::ui::UIFlowScreenDefinition{
            screenId,
            world.ui,
            kLegacyLayerId,
            kLegacySlotId,
            ayt::ui::UIFlowScope::World,
        });
        generated.contexts.push_back(ayt::ui::UIFlowContextDefinition{
            contextId,
            0,
            {ayt::ui::UIFlowSlotAssignment{
                kLegacySlotId,
                ayt::ui::UIFlowSlotOperation::Present,
                screenId}},
        });
        resolution.worldContexts.push_back(
            EditorProjectWorldUiContextBinding{world.id, contextId});
    }

    if (generated.screens.empty()) {
        resolution = {};
        return true;
    }

    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    if (!ayt::ui::validateUIFlow(generated, &diagnostics)) {
        if (error != nullptr) *error = firstDiagnosticMessage(diagnostics);
        resolution = {};
        return false;
    }

    resolution.source = EditorProjectUiFlowSource::LegacyWorldLayouts;
    resolution.compatibilityFlow = std::move(generated);
    return true;
}

} // namespace ayt::editor
